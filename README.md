# Listen

Small Windows tray app to toggle listening to an audio input device.
Pipes a chosen capture device to the default audio render device. (My personal use case is to easily listen to my USB connected turntable.)

## Features

- Sits in the system tray; double-click to toggle playback on/off
- Lists all available audio capture devices via the tray context menu
- Starts muted by default — enable listening only when you want it
- Sets a configurable volume level (0–100, default 70) on the input endpoint at startup and on every toggle
- Persists the last-used device and volume to the registry (`HKCU\Software\Listen`)
- Optional **Run on login** toggle (registers under `HKCU\...\Run\Listen`)
- Promotes its own tray icon to "always show" so it is never hidden
- No installer, no runtime dependencies — single static executable

## Usage

| Action | Result |
|---|---|
| Double-click tray icon | Toggle listening on / off |
| Right-click tray icon | Open context menu |
| Context menu → *(device name)* | Switch input device |
| Context menu → Run on login | Enable / disable autostart |
| Context menu → Exit | Quit the app |

## Building

**Requires:** [w64devkit](https://github.com/skeeto/w64devkit) (or any MinGW-w64 toolchain with `g++` and `windres` on `PATH`).

```bat
build.bat
```

This compiles `main.cpp` and the embedded resources into a single statically linked `Listen.exe`. No external libraries are needed beyond stock Windows DLLs (`ole32`, `user32`, `shell32`, `advapi32`, `avrt`).

## Requirements

- Windows 10 or later
- An audio capture device (microphone, line-in, etc.)
