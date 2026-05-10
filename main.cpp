// Listen — minimal Windows tray app that pipes a chosen audio capture device
// to the current default audio render device. Single-file, statically linked.
//
// Behaviour:
//   - Optionally registers itself for current-user login via a "Run on login"
//     tray-menu toggle (HKCU ...\Run\Listen); off by default.
//   - Self-promotes its tray icon to "always show in taskbar" (HKCU
//     ...\NotifyIconSettings\<uid>\IsPromoted = 1).
//   - Always launches with playback muted; double-click toggles playback.
//   - Applies a volume level (0-100, default 70) to the input endpoint on
//     startup and on every mute/unmute toggle.
//   - Saves last-used input device id and volume to HKCU\Software\Listen.
//
// Build: see build.bat
// Deps : stock Windows DLLs only (ole32, user32, shell32, advapi32, avrt).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <strsafe.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

#include "resource.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const wchar_t kAppName[]       = L"Listen";
static const wchar_t kWindowClass[]   = L"ListenTrayHidden";
static const int     kDefaultVolume   = 70;

static const wchar_t kRunRegPath[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValueName[]  = L"Listen";
static const wchar_t kNotifyRegPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\NotifyIconSettings";

// Stable GUID for the tray icon (lets Windows persist user prefs per exe path).
// {6A1E2C4D-9F0B-4C7E-AE1F-1C7B1F2B3D4A}
static const GUID kTrayGuid =
    { 0x6a1e2c4d, 0x9f0b, 0x4c7e, { 0xae, 0x1f, 0x1c, 0x7b, 0x1f, 0x2b, 0x3d, 0x4a } };

#define WM_TRAYICON         (WM_APP + 1)
#define WM_TRAY_REFRESH     (WM_APP + 2)
#define TIMER_PROMOTE_ICON  1

#define IDM_EXIT            1001
#define IDM_TOGGLE_MUTE     1002
#define IDM_TOGGLE_AUTORUN  1003
#define IDM_DEVICE_FIRST    2000
#define IDM_DEVICE_LAST     2999

// Buffer duration in 100-ns units (20 ms).
static const REFERENCE_TIME kBufferDuration = 200000;

// ---------------------------------------------------------------------------
// Small RAII helpers
// ---------------------------------------------------------------------------

template <class T>
class ComPtr {
public:
    ComPtr() : p_(nullptr) {}
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~ComPtr() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T**  put()       { reset(); return &p_; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }
    T*   get() const { return p_; }
    T*   operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_;
};

struct CoMemFree {
    void operator()(void* p) const { if (p) CoTaskMemFree(p); }
};
template <class T>
using CoMem = std::unique_ptr<T, CoMemFree>;

struct PropVar {
    PROPVARIANT pv;
    PropVar()  { PropVariantInit(&pv); }
    ~PropVar() { PropVariantClear(&pv); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

static HWND                     g_hWnd            = nullptr;
static HICON                    g_hIconOn         = nullptr;
static HICON                    g_hIconOff        = nullptr;
static bool                     g_iconsOwned      = false;
static std::wstring             g_exePath;

static std::mutex               g_engineMutex;
static std::wstring             g_selectedDeviceId;
static std::wstring             g_selectedDeviceName;

static HANDLE                   g_workerThread    = nullptr;
static HANDLE                   g_stopEvent       = nullptr;
static HANDLE                   g_restartEvent    = nullptr;
static HANDLE                   g_renderRebuild   = nullptr;
static HANDLE                   g_playbackToggle  = nullptr; // signals pump to sync capture state

// Playback gate (replaces endpoint mute). false = muted (silent), true = passing audio.
static std::atomic<bool>        g_playbackEnabled{false};

// Volume percent (0-100), loaded from registry; applied on startup and every toggle.
static std::atomic<int>         g_volumePercent{kDefaultVolume};

// Whether the app is registered to run on login (HKCU\...\Run\Listen).
static std::atomic<bool>        g_runOnLogin{false};

static std::vector<DeviceInfo>  g_lastDeviceList;
static int                      g_promoteRetries  = 0;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void DbgF(const wchar_t* fmt, ...) {
#ifdef _DEBUG
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    StringCchVPrintfW(buf, 1024, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
#else
    (void)fmt;
#endif
}

// ---------------------------------------------------------------------------
// Path helper
// ---------------------------------------------------------------------------

static std::wstring GetExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L"";
    return std::wstring(buf, n);
}

// ---------------------------------------------------------------------------
// Registry: settings (HKCU\Software\Listen)
// ---------------------------------------------------------------------------

static const wchar_t kSettingsRegPath[] = L"Software\\Listen";

static std::wstring LoadSavedDeviceId() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    RegQueryValueExW(hk, L"InputDeviceId", nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hk);
    return (type == REG_SZ || type == REG_EXPAND_SZ) ? std::wstring(buf) : L"";
}

static void SaveDeviceId(const std::wstring& id) {
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
    DWORD bytes = (DWORD)((id.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(hk, L"InputDeviceId", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(id.c_str()), bytes);
    RegCloseKey(hk);
}

static int LoadVolume() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return kDefaultVolume;
    DWORD val = 0, cb = sizeof(val), type = 0;
    LONG r = RegQueryValueExW(hk, L"Volume", nullptr, &type,
                              reinterpret_cast<BYTE*>(&val), &cb);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return kDefaultVolume;
    int v = (int)val;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void SaveVolumeIfMissing(int v) {
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegPath, 0, nullptr,
                        0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &hk, &disp) != ERROR_SUCCESS) return;
    // Only write if the value is absent (disp == REG_CREATED_NEW_KEY covers the
    // whole key being new; for existing keys, check whether the value exists).
    bool absent = (disp == REG_CREATED_NEW_KEY);
    if (!absent) {
        DWORD type = 0, cb = 0;
        absent = (RegQueryValueExW(hk, L"Volume", nullptr, &type, nullptr, &cb) != ERROR_SUCCESS);
    }
    if (absent) {
        DWORD dv = (DWORD)v;
        RegSetValueExW(hk, L"Volume", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&dv), sizeof(dv));
    }
    RegCloseKey(hk);
}

// ---------------------------------------------------------------------------
// Registry: auto-run + tray-icon promotion
// ---------------------------------------------------------------------------

// FNV-1a 32-bit over case-folded wide path; produces a stable UID per exe path.
static DWORD HashExePathToUid(const std::wstring& path) {
    uint32_t h = 0x811c9dc5u;
    for (wchar_t wc : path) {
        wchar_t c = (wc >= L'A' && wc <= L'Z') ? (wchar_t)(wc - L'A' + L'a') : wc;
        h ^= (uint32_t)(c & 0xFF);
        h *= 0x01000193u;
        h ^= (uint32_t)((c >> 8) & 0xFF);
        h *= 0x01000193u;
    }
    if (h == 0) h = 1;
    return (DWORD)h;
}

// Ensure HKCU\...\Run\Listen = "<exe path>". Always overwrites; single value only.
static void RegisterAutoRun() {
    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr,
                             0, KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) return;

    // Quote path so spaces survive parsing by Explorer.
    std::wstring quoted = L"\"" + g_exePath + L"\"";
    DWORD bytes = (DWORD)((quoted.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(hk, kRunValueName, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(quoted.c_str()), bytes);
    RegCloseKey(hk);
}

// Returns true if HKCU\...\Run\Listen exists AND points at the current exe.
// A stale value from a different install path is treated as absent.
static bool CheckAutoRunExists() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0,
                      KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;

    // MAX_PATH + 4: enough for the quoted form "\"<MAX_PATH path>\"" + null.
    wchar_t buf[MAX_PATH + 4] = {};
    DWORD cb   = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LONG r = RegQueryValueExW(hk, kRunValueName, nullptr, &type,
                              reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hk);

    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;

    // Stored as "\"<path>\"" — strip surrounding quotes before comparing.
    std::wstring stored(buf);
    if (!stored.empty() && stored.front() == L'"') stored.erase(stored.begin());
    if (!stored.empty() && stored.back()  == L'"') stored.pop_back();
    return _wcsicmp(stored.c_str(), g_exePath.c_str()) == 0;
}

// Remove HKCU\...\Run\Listen only. All other Run values are left untouched.
static void UnregisterAutoRun() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0,
                      KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(hk, kRunValueName);
    RegCloseKey(hk);
}

// Write/update a single NotifyIconSettings entry forcing IsPromoted=1.
// If an existing entry references our exe path, update it (keeps shell happy).
// Otherwise create one keyed by FNV-1a hash of the exe path.
// Returns true once IsPromoted is set on at least one matching entry.
static bool PromoteTrayIcon() {
    bool didAny = false;

    // 1) Look for an existing entry that points at us.
    HKEY hRoot = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyRegPath, 0, KEY_READ, &hRoot) == ERROR_SUCCESS) {
        DWORD idx = 0;
        for (;; ++idx) {
            wchar_t sub[64];
            DWORD subLen = ARRAYSIZE(sub);
            LONG e = RegEnumKeyExW(hRoot, idx, sub, &subLen, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS) break;

            HKEY hChild = nullptr;
            if (RegOpenKeyExW(hRoot, sub, 0, KEY_READ | KEY_SET_VALUE, &hChild) != ERROR_SUCCESS) continue;

            wchar_t exePath[MAX_PATH * 2] = {};
            DWORD type = 0;
            DWORD cb = sizeof(exePath) - sizeof(wchar_t);
            if (RegQueryValueExW(hChild, L"ExecutablePath", nullptr, &type,
                                 reinterpret_cast<BYTE*>(exePath), &cb) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {

                wchar_t expanded[MAX_PATH * 2];
                DWORD n = ExpandEnvironmentStringsW(exePath, expanded, ARRAYSIZE(expanded));
                const wchar_t* effective = (n > 0 && n <= ARRAYSIZE(expanded)) ? expanded : exePath;

                if (_wcsicmp(effective, g_exePath.c_str()) == 0) {
                    DWORD one = 1;
                    RegSetValueExW(hChild, L"IsPromoted", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&one), sizeof(one));
                    didAny = true;
                }
            }
            RegCloseKey(hChild);
        }
        RegCloseKey(hRoot);
    }

    // 2) Always (re)write our own canonical entry as well, so the setting
    //    survives even if Windows hasn't yet created an Explorer-managed one.
    DWORD uid = HashExePathToUid(g_exePath);
    wchar_t subName[16];
    StringCchPrintfW(subName, 16, L"%lu", uid);

    std::wstring fullPath = std::wstring(kNotifyRegPath) + L"\\" + subName;
    HKEY hOurs = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, fullPath.c_str(), 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &hOurs, nullptr) == ERROR_SUCCESS) {
        DWORD bytes = (DWORD)((g_exePath.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(hOurs, L"ExecutablePath", 0, REG_EXPAND_SZ,
                       reinterpret_cast<const BYTE*>(g_exePath.c_str()), bytes);

        DWORD one = 1;
        RegSetValueExW(hOurs, L"IsPromoted", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegSetValueExW(hOurs, L"UID", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&uid), sizeof(uid));
        RegCloseKey(hOurs);
        didAny = true;
    }
    return didAny;
}

// ---------------------------------------------------------------------------
// Tray icon loading from SndVol.exe
// ---------------------------------------------------------------------------

static void FreeIcons() {
    if (!g_iconsOwned) return;
    if (g_hIconOn)  { DestroyIcon(g_hIconOn);  g_hIconOn  = nullptr; }
    if (g_hIconOff) { DestroyIcon(g_hIconOff); g_hIconOff = nullptr; }
    g_iconsOwned = false;
}

// Loads icons live from SndVol.exe so they always reflect the installed copy.
// Index 1 = unmuted speaker, index 2 = muted speaker (0-based numbering in the file).
static void LoadSndVolIcons() {
    FreeIcons();

    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\SndVol.exe", path, MAX_PATH);

    HICON on = nullptr, off = nullptr;
    ExtractIconExW(path, 1, &on,  nullptr, 1);
    ExtractIconExW(path, 2, &off, nullptr, 1);

    if (on && off) {
        g_hIconOn    = on;
        g_hIconOff   = off;
        g_iconsOwned = true;
    } else {
        if (on)  DestroyIcon(on);
        if (off) DestroyIcon(off);
        // Fallback to stock Windows icons (not owned — do not DestroyIcon).
        g_hIconOn    = LoadIconW(nullptr, IDI_INFORMATION);
        g_hIconOff   = LoadIconW(nullptr, IDI_APPLICATION);
        g_iconsOwned = false;
    }
}

// ---------------------------------------------------------------------------
// Volume application (input endpoint master volume)
// ---------------------------------------------------------------------------

static void ApplyVolumeToSelectedDevice() {
    std::wstring id;
    {
        std::lock_guard<std::mutex> g(g_engineMutex);
        id = g_selectedDeviceId;
    }
    if (id.empty()) return;

    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(enumr.put())))) return;
    ComPtr<IMMDevice> dev;
    if (FAILED(enumr->GetDevice(id.c_str(), dev.put()))) return;
    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             vol.putVoid()))) return;

    int v = g_volumePercent.load();
    if (v < 0) v = 0; if (v > 100) v = 100;
    float scalar = (float)v / 100.0f;
    vol->SetMasterVolumeLevelScalar(scalar, nullptr);
}

// ---------------------------------------------------------------------------
// Device name normalization
// ---------------------------------------------------------------------------

// Trims leading/trailing whitespace. Returns "Unnamed Input Device" if blank.
// Used when storing names — preserves the full "Friendly (Device)" form for
// display in the menu.
static std::wstring TrimName(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"Unnamed Input Device";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Derives the short name shown in the tooltip from the stored full name.
// Shows only the friendly part (before '('). Falls back to the device part
// (inside the parens) if the friendly part is blank, or "Unnamed Input Device"
// if both parts are blank.
static std::wstring TooltipName(const std::wstring& raw) {
    auto trim = [](const std::wstring& s) -> std::wstring {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    size_t paren = raw.find(L'(');
    if (paren == std::wstring::npos)
        return raw.empty() ? L"Unnamed Input Device" : raw; // already trimmed

    std::wstring friendly = trim(raw.substr(0, paren));

    // Extract the content between '(' and the last ')'.
    std::wstring devPart;
    size_t close = raw.rfind(L')');
    if (close != std::wstring::npos && close > paren)
        devPart = trim(raw.substr(paren + 1, close - paren - 1));
    else
        devPart = trim(raw.substr(paren + 1));

    if (!friendly.empty()) return friendly;
    if (!devPart.empty())  return devPart;
    return L"Unnamed Input Device";
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

static std::vector<DeviceInfo> EnumerateCaptureDevices() {
    std::vector<DeviceInfo> result;
    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(enumr.put()))))
        return result;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumr->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, coll.put())))
        return result;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put()))) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(dev->GetId(&rawId))) continue;
        CoMem<wchar_t> idGuard(rawId);

        std::wstring name;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PropVar v;
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v.pv))
                && v.pv.vt == VT_LPWSTR && v.pv.pwszVal) {
                name = v.pv.pwszVal;
            }
        }
        result.push_back({ rawId, TrimName(name) });
    }
    return result;
}

// ---------------------------------------------------------------------------
// IMMNotificationClient — tracks default render device + selected capture state
// ---------------------------------------------------------------------------

class NotificationClient : public IMMNotificationClient {
public:
    NotificationClient() : ref_(1) {}
    virtual ~NotificationClient() = default;

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            if (g_renderRebuild) SetEvent(g_renderRebuild);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR)   override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        std::lock_guard<std::mutex> g(g_engineMutex);
        if (!g_selectedDeviceId.empty() && id &&
            _wcsicmp(id, g_selectedDeviceId.c_str()) == 0) {
            if (g_restartEvent) SetEvent(g_restartEvent);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_;
};

// ---------------------------------------------------------------------------
// Audio engine worker thread
// ---------------------------------------------------------------------------

namespace engine {

struct StreamCtx {
    ComPtr<IAudioClient>         client;
    ComPtr<IAudioCaptureClient>  capture;
    ComPtr<IAudioRenderClient>   render;
    HANDLE                       evt = nullptr;
    UINT32                       bufferFrames = 0;
    WAVEFORMATEX*                fmtRaw = nullptr;
    CoMem<WAVEFORMATEX>          fmtOwner;
};

static void Cleanup(StreamCtx& s) {
    if (s.client) s.client->Stop();
    s.capture.reset();
    s.render.reset();
    s.client.reset();
    s.fmtOwner.reset();
    s.fmtRaw = nullptr;
    if (s.evt) { CloseHandle(s.evt); s.evt = nullptr; }
}

static HRESULT InitCapture(IMMDevice* dev, StreamCtx& cap) {
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               cap.client.putVoid());
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* mix = nullptr;
    hr = cap.client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    cap.fmtOwner.reset(mix);
    cap.fmtRaw = mix;

    hr = cap.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kBufferDuration, 0, mix, nullptr);
    if (FAILED(hr)) return hr;

    cap.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!cap.evt) return HRESULT_FROM_WIN32(GetLastError());
    hr = cap.client->SetEventHandle(cap.evt);
    if (FAILED(hr)) return hr;

    hr = cap.client->GetBufferSize(&cap.bufferFrames);
    if (FAILED(hr)) return hr;

    hr = cap.client->GetService(IID_PPV_ARGS(cap.capture.put()));
    // NOTE: caller is responsible for Start()/Stop() to control the activity indicator.
    return hr;
}

// Render uses the same format as capture; WASAPI converts via AUTOCONVERTPCM.
// No event callback needed — the pump is driven entirely by the capture event.
static HRESULT InitRender(IMMDevice* dev, const WAVEFORMATEX* fmt, StreamCtx& ren) {
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               ren.client.putVoid());
    if (FAILED(hr)) return hr;

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = ren.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                kBufferDuration, 0, fmt, nullptr);
    if (FAILED(hr)) return hr;

    hr = ren.client->GetBufferSize(&ren.bufferFrames);
    if (FAILED(hr)) return hr;

    hr = ren.client->GetService(IID_PPV_ARGS(ren.render.put()));
    if (FAILED(hr)) return hr;

    return ren.client->Start();
}

static HRESULT OpenCaptureDevice(IMMDeviceEnumerator* enumr,
                                 const std::wstring& wantedId,
                                 ComPtr<IMMDevice>& out,
                                 std::wstring& outId,
                                 std::wstring& outName) {
    HRESULT hr = E_FAIL;
    if (!wantedId.empty()) {
        hr = enumr->GetDevice(wantedId.c_str(), out.put());
        if (SUCCEEDED(hr)) {
            DWORD st = 0;
            if (SUCCEEDED(out->GetState(&st)) && st == DEVICE_STATE_ACTIVE) {
                outId = wantedId;
            } else {
                out.reset();
                hr = E_FAIL;
            }
        }
    }
    if (FAILED(hr)) {
        hr = enumr->GetDefaultAudioEndpoint(eCapture, eMultimedia, out.put());
        if (FAILED(hr)) return hr;
        LPWSTR raw = nullptr;
        if (SUCCEEDED(out->GetId(&raw))) {
            outId.assign(raw);
            CoTaskMemFree(raw);
        }
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(out->OpenPropertyStore(STGM_READ, props.put()))) {
        PropVar v;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v.pv))
            && v.pv.vt == VT_LPWSTR && v.pv.pwszVal) {
            outName.assign(TrimName(v.pv.pwszVal));
        }
    }
    if (outName.empty()) outName = L"Unnamed Input Device";
    return S_OK;
}

// Returns true if asked to fully stop, false if it should rebuild capture.
static bool RunSession(IMMDeviceEnumerator* enumr) {
    std::wstring wantedId;
    {
        std::lock_guard<std::mutex> g(g_engineMutex);
        wantedId = g_selectedDeviceId;
    }

    ComPtr<IMMDevice> capDev;
    std::wstring usedId, usedName;
    HRESULT hr = OpenCaptureDevice(enumr, wantedId, capDev, usedId, usedName);
    if (FAILED(hr) || !capDev) {
        DbgF(L"[Listen] OpenCaptureDevice failed 0x%08lx\n", hr);
        HANDLE waits[] = { g_stopEvent, g_restartEvent };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        return (w == WAIT_OBJECT_0);
    }

    {
        std::lock_guard<std::mutex> g(g_engineMutex);
        g_selectedDeviceId   = usedId;
        g_selectedDeviceName = usedName;
    }
    SaveDeviceId(usedId);

    // Notify the UI thread to refresh the tray tooltip with the resolved name.
    if (g_hWnd) PostMessageW(g_hWnd, WM_TRAY_REFRESH, 0, 0);

    // Apply configured volume now that the input endpoint is known.
    ApplyVolumeToSelectedDevice();

    StreamCtx cap;
    hr = InitCapture(capDev.get(), cap);
    if (FAILED(hr)) {
        DbgF(L"[Listen] InitCapture failed 0x%08lx\n", hr);
        Cleanup(cap);
        HANDLE waits[] = { g_stopEvent, g_restartEvent };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        return (w == WAIT_OBJECT_0);
    }

    ComPtr<IMMDevice> renDev;
    StreamCtx ren;
    auto buildRender = [&](void) -> HRESULT {
        Cleanup(ren);
        renDev.reset();
        HRESULT h = enumr->GetDefaultAudioEndpoint(eRender, eConsole, renDev.put());
        if (FAILED(h)) return h;
        return InitRender(renDev.get(), cap.fmtRaw, ren);
    };

    hr = buildRender();
    if (FAILED(hr)) {
        DbgF(L"[Listen] InitRender failed 0x%08lx\n", hr);
        HANDLE waits[] = { g_stopEvent, g_restartEvent, g_renderRebuild };
        DWORD w = WaitForMultipleObjects(3, waits, FALSE, 2000);
        Cleanup(cap);
        return (w == WAIT_OBJECT_0);
    }

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const UINT32 frameBytes = cap.fmtRaw->nBlockAlign;
    bool fullStop = false;

    // Start capture only if playback is already enabled; otherwise leave the
    // client stopped so the OS input-device activity indicator stays off.
    bool capActive = g_playbackEnabled.load() && SUCCEEDED(cap.client->Start());
    DbgF(L"[Listen] session start: capActive=%d\n", (int)capActive);

    for (;;) {
        // Only include cap.evt in the wait set while the client is running;
        // a stopped client never signals it.
        HANDLE waits[] = { g_stopEvent, g_restartEvent, g_renderRebuild,
                           g_playbackToggle, cap.evt };
        DWORD w = WaitForMultipleObjects(capActive ? 5 : 4, waits, FALSE, 1000);

        if (w == WAIT_OBJECT_0)     { fullStop = true; break; }
        if (w == WAIT_OBJECT_0 + 1) { break; }
        if (w == WAIT_OBJECT_0 + 2) {
            HRESULT rh = buildRender();
            if (FAILED(rh)) {
                DbgF(L"[Listen] rebuild render failed 0x%08lx\n", rh);
                UINT32 packet = 0;
                while (cap.capture && SUCCEEDED(cap.capture->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* d; UINT32 nf; DWORD fl; UINT64 dp, qp;
                    if (SUCCEEDED(cap.capture->GetBuffer(&d, &nf, &fl, &dp, &qp))) {
                        cap.capture->ReleaseBuffer(nf);
                    } else break;
                }
            }
            continue;
        }
        if (w == WAIT_OBJECT_0 + 3) {
            // Playback was toggled — start or stop the capture client.
            const bool playing = g_playbackEnabled.load();
            if (playing && !capActive) {
                if (SUCCEEDED(cap.client->Start())) {
                    capActive = true;
                } else {
                    // Device became unavailable; restart the session.
                    if (g_restartEvent) SetEvent(g_restartEvent);
                }
            } else if (!playing && capActive) {
                cap.client->Stop();
                // Drain any frames already queued so the buffer is clean on restart.
                UINT32 p = 0;
                while (cap.capture && SUCCEEDED(cap.capture->GetNextPacketSize(&p)) && p > 0) {
                    BYTE* d; UINT32 nf; DWORD fl; UINT64 dp, qp;
                    if (SUCCEEDED(cap.capture->GetBuffer(&d, &nf, &fl, &dp, &qp)))
                        cap.capture->ReleaseBuffer(nf);
                    else break;
                }
                capActive = false;
            }
            continue;
        }
        if (w == WAIT_TIMEOUT) continue;
        if (w == WAIT_FAILED)  { fullStop = true; break; }

        // WAIT_OBJECT_0 + 4: cap.evt fired — capture buffer is ready.
        // capActive must be true here; forward audio to render.
        UINT32 packetSize = 0;
        while (SUCCEEDED(cap.capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            UINT64 devPos = 0, qpcPos = 0;
            HRESULT gh = cap.capture->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
            if (gh == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(gh)) {
                if (gh == AUDCLNT_E_DEVICE_INVALIDATED) {
                    if (g_restartEvent) SetEvent(g_restartEvent);
                }
                break;
            }

            if (ren.render && ren.client) {
                UINT32 padding = 0;
                if (SUCCEEDED(ren.client->GetCurrentPadding(&padding))) {
                    UINT32 avail = ren.bufferFrames - padding;
                    UINT32 toWrite = (frames < avail) ? frames : avail;
                    if (toWrite > 0) {
                        BYTE* renBuf = nullptr;
                        HRESULT rh = ren.render->GetBuffer(toWrite, &renBuf);
                        if (SUCCEEDED(rh)) {
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                ZeroMemory(renBuf, (size_t)toWrite * frameBytes);
                            } else {
                                memcpy(renBuf, data, (size_t)toWrite * frameBytes);
                            }
                            ren.render->ReleaseBuffer(toWrite, 0);
                        } else if (rh == AUDCLNT_E_DEVICE_INVALIDATED) {
                            if (g_renderRebuild) SetEvent(g_renderRebuild);
                        }
                    }
                }
            }

            cap.capture->ReleaseBuffer(frames);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    Cleanup(ren);
    Cleanup(cap);
    return fullStop;
}

static DWORD WINAPI WorkerThreadProc(LPVOID) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) return 1;

    ComPtr<IMMDeviceEnumerator> enumr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(enumr.put())))) {
        CoUninitialize();
        return 2;
    }

    NotificationClient* notify = new NotificationClient();
    enumr->RegisterEndpointNotificationCallback(notify);

    for (;;) {
        bool stop = RunSession(enumr.get());
        if (stop) break;
        // g_restartEvent is auto-reset; WaitForMultipleObjects already consumed
        // the signal. No ResetEvent needed here.
    }

    enumr->UnregisterEndpointNotificationCallback(notify);
    notify->Release();
    enumr.reset();
    CoUninitialize();
    return 0;
}

} // namespace engine

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

static void BuildTip(wchar_t* out, size_t cch) {
    std::wstring name;
    {
        std::lock_guard<std::mutex> g(g_engineMutex);
        name = g_selectedDeviceName;
    }
    // g_selectedDeviceName stores the full trimmed name; derive the short form
    // (friendly part only) for the tooltip here.
    std::wstring tip = name.empty() ? L"(no device)" : TooltipName(name);
    StringCchCopyW(out, cch, tip.c_str());
}

static void TrayAdd() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hWnd;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
    nid.guidItem         = kTrayGuid;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = g_playbackEnabled.load() ? g_hIconOn : g_hIconOff;
    BuildTip(nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void TrayUpdate() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize   = sizeof(nid);
    nid.hWnd     = g_hWnd;
    nid.uFlags   = NIF_ICON | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
    nid.guidItem = kTrayGuid;
    nid.hIcon    = g_playbackEnabled.load() ? g_hIconOn : g_hIconOff;
    BuildTip(nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayRemove() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize   = sizeof(nid);
    nid.hWnd     = g_hWnd;
    nid.uFlags   = NIF_GUID;
    nid.guidItem = kTrayGuid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------------------------------------------------------------------------
// Playback toggle (replaces endpoint-mute toggle)
// ---------------------------------------------------------------------------

static void TogglePlayback() {
    bool nowPlaying = !g_playbackEnabled.load();
    g_playbackEnabled.store(nowPlaying);
    // Volume is reapplied on every toggle so the registry value takes effect promptly.
    ApplyVolumeToSelectedDevice();
    // Wake the worker thread immediately so it starts/stops capture without
    // waiting for the 1 s heartbeat timeout.
    if (g_playbackToggle) SetEvent(g_playbackToggle);
    TrayUpdate();
}

// ---------------------------------------------------------------------------
// Tray menu
// ---------------------------------------------------------------------------

static void ShowTrayMenu(POINT pt) {
    g_lastDeviceList = EnumerateCaptureDevices();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    UINT autoRunFlags = MF_STRING;
    if (g_runOnLogin.load()) autoRunFlags |= MF_CHECKED;
    AppendMenuW(menu, autoRunFlags, IDM_TOGGLE_AUTORUN, L"Run on login");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    std::wstring selectedId;
    {
        std::lock_guard<std::mutex> g(g_engineMutex);
        selectedId = g_selectedDeviceId;
    }

    if (g_lastDeviceList.empty()) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    L"No input devices available");
    } else {
        for (size_t i = 0; i < g_lastDeviceList.size() &&
                          i < (IDM_DEVICE_LAST - IDM_DEVICE_FIRST); ++i) {
            UINT flags = MF_STRING;
            if (_wcsicmp(g_lastDeviceList[i].id.c_str(), selectedId.c_str()) == 0) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(menu, flags, IDM_DEVICE_FIRST + (UINT)i,
                        g_lastDeviceList[i].name.c_str());
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT muteFlags = MF_STRING;
    if (!g_playbackEnabled.load()) muteFlags |= MF_CHECKED;
    if (selectedId.empty()) muteFlags |= MF_DISABLED | MF_GRAYED;
    AppendMenuW(menu, muteFlags, IDM_TOGGLE_MUTE, L"Mute");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(g_hWnd);
    UINT cmd = TrackPopupMenu(menu,
                              TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g_hWnd, nullptr);
    PostMessageW(g_hWnd, WM_NULL, 0, 0); // standard menu-dismiss workaround
    DestroyMenu(menu);

    if (cmd == 0) return;
    if (cmd == IDM_EXIT)         { DestroyWindow(g_hWnd); return; }
    if (cmd == IDM_TOGGLE_MUTE)  { TogglePlayback();      return; }
    if (cmd == IDM_TOGGLE_AUTORUN) {
        bool nowOn = !g_runOnLogin.load();
        g_runOnLogin.store(nowOn);
        if (nowOn) RegisterAutoRun(); else UnregisterAutoRun();
        return;
    }
    if (cmd >= IDM_DEVICE_FIRST && cmd <= IDM_DEVICE_LAST) {
        size_t idx = cmd - IDM_DEVICE_FIRST;
        if (idx < g_lastDeviceList.size()) {
            {
                std::lock_guard<std::mutex> g(g_engineMutex);
                g_selectedDeviceId   = g_lastDeviceList[idx].id;
                g_selectedDeviceName = g_lastDeviceList[idx].name;
            }
            SaveDeviceId(g_lastDeviceList[idx].id);
            // New device — re-mute per startup policy and let engine restart.
            g_playbackEnabled.store(false);
            if (g_restartEvent) SetEvent(g_restartEvent);
            TrayUpdate();
        }
    }
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = 0;
    if (s_taskbarCreated == 0) {
        s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    }
    if (msg == s_taskbarCreated) {
        // Explorer restarted — reload icons fresh from SndVol.exe, then re-add.
        LoadSndVolIcons();
        TrayAdd();
        g_promoteRetries = 0;
        PromoteTrayIcon();
        SetTimer(hWnd, TIMER_PROMOTE_ICON, 1500, nullptr);
        return 0;
    }

    switch (msg) {
    case WM_TRAY_REFRESH:
        TrayUpdate();
        return 0;
    case WM_TRAYICON: {
        UINT event = LOWORD(lp);
        switch (event) {
        case WM_LBUTTONDBLCLK:
            TogglePlayback();
            break;
        case WM_CONTEXTMENU: {
            // WM_RBUTTONUP is never delivered with NOTIFYICON_VERSION_4;
            // the shell synthesizes WM_CONTEXTMENU instead.
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(pt);
            break;
        }
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_PROMOTE_ICON) {
            // Retry once or twice in case Windows hadn't created the icon's
            // entry yet at the moment we first wrote IsPromoted.
            PromoteTrayIcon();
            if (++g_promoteRetries >= 2) {
                KillTimer(hWnd, TIMER_PROMOTE_ICON);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hWnd, TIMER_PROMOTE_ICON);
        TrayRemove();
        FreeIcons();
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_workerThread) {
            WaitForSingleObject(g_workerThread, 3000);
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

static int RunApp(HINSTANCE hInst) {
    g_exePath = GetExePath();

    // Read current auto-run state from registry; do not modify it on startup.
    g_runOnLogin.store(CheckAutoRunExists());

    // Load icons live from SndVol.exe.
    LoadSndVolIcons();

    // Load saved settings
    g_selectedDeviceId = LoadSavedDeviceId();
    g_volumePercent.store(LoadVolume());
    SaveVolumeIfMissing(g_volumePercent.load());

    // Create message-only window
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    g_hWnd = CreateWindowExW(0, kWindowClass, kAppName, 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hWnd) return 2;

    g_stopEvent      = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    g_restartEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_renderRebuild  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_playbackToggle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_restartEvent || !g_renderRebuild || !g_playbackToggle) {
        if (g_stopEvent)      { CloseHandle(g_stopEvent);      g_stopEvent      = nullptr; }
        if (g_restartEvent)   { CloseHandle(g_restartEvent);   g_restartEvent   = nullptr; }
        if (g_renderRebuild)  { CloseHandle(g_renderRebuild);  g_renderRebuild  = nullptr; }
        if (g_playbackToggle) { CloseHandle(g_playbackToggle); g_playbackToggle = nullptr; }
        return 4;
    }

    TrayAdd();

    // Promote the tray icon to "always show in taskbar".
    PromoteTrayIcon();
    SetTimer(g_hWnd, TIMER_PROMOTE_ICON, 1500, nullptr);

    g_workerThread = CreateThread(nullptr, 0, engine::WorkerThreadProc, nullptr, 0, nullptr);
    if (!g_workerThread) {
        TrayRemove();
        return 3;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_stopEvent)      CloseHandle(g_stopEvent);
    if (g_restartEvent)   CloseHandle(g_restartEvent);
    if (g_renderRebuild)  CloseHandle(g_renderRebuild);
    if (g_playbackToggle) CloseHandle(g_playbackToggle);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"Local\\Listen.SingleInstance.6A1E2C4D");
    if (!mtx) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mtx);
        return 0;
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = RunApp(hInst);
    if (SUCCEEDED(hrInit)) CoUninitialize();
    CloseHandle(mtx);
    return rc;
}
