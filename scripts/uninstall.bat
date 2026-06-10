@echo off
setlocal

:: Self-elevate to administrator if not already
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process cmd -Verb RunAs -ArgumentList '/c \"%~f0\"'"
    exit /b
)

cd /d "%~dp0"

echo.
echo  PSVR2 Passthrough Layer - Uninstalling...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_layer.ps1"

if %errorlevel% neq 0 (
    echo.
    echo  ERROR: Uninstall failed. See above for details.
    echo.
    pause
    exit /b 1
)

echo.
echo  ============================================================
echo   Uninstall complete!
echo.
echo   - OpenXR layer unregistered (HKLM/HKCU).
echo   - OpenVR overlay unregistered from SteamVR (autolaunch off).
echo     If SteamVR was not running just now, start it and run:
echo       PSVR2PassthroughOverlay.exe --unregister
echo.
echo   Your settings file is left in:
echo     %%LOCALAPPDATA%%\PSVR2PassthroughLayer\
echo   Delete that folder if you want to remove it too.
echo  ============================================================
echo.
pause
