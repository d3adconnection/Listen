@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
  echo [ERROR] g++ not found on PATH. Open the w64devkit shell ^(w64devkit.bat^) and run this script from there.
  exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
  echo [ERROR] windres not found on PATH.
  exit /b 1
)

echo [1/3] Compiling resources...
windres app.rc -O coff -o app.res
if errorlevel 1 exit /b 1

echo [2/3] Compiling and linking Listen.exe...
g++ -std=c++17 -Os -municode -mwindows ^
    -ffunction-sections -fdata-sections ^
    -fno-exceptions -fno-rtti ^
    main.cpp app.res -o Listen.exe ^
    -lole32 -luuid -lavrt -lwinmm -lshell32 -luser32 -ladvapi32 -lcomctl32 ^
    -static -static-libgcc -static-libstdc++ ^
    -Wl,--gc-sections -Wl,-s
if errorlevel 1 exit /b 1

echo [3/3] Done.
dir Listen.exe | find "Listen.exe"
echo.
echo Dependencies (should be only stock Windows DLLs):
objdump -p Listen.exe 2>nul | findstr /R "DLL.Name:"
endlocal
