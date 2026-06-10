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
echo  PSVR2 Passthrough Layer - Installing...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_layer.ps1"

if %errorlevel% neq 0 (
    echo.
    echo  ERROR: Installation failed. See above for details.
    echo.
    pause
    exit /b 1
)

echo.
echo  ============================================================
echo   Installation complete!
echo.
echo   - OpenXR layer registered (DCS, MSFS, other OpenXR games).
echo   - OpenVR overlay registered with SteamVR for native-OpenVR
echo     games (Half-Life: Alyx, No Man's Sky). It autostarts with
echo     SteamVR. If SteamVR was not running just now, start it and
echo     run:  PSVR2PassthroughOverlay.exe --register
echo.
echo   Opening the Configuration app.
echo   Set your passthrough toggle key or button before launching
echo   your sim.
echo  ============================================================
echo.
pause

start "" "%~dp0PSVR2PassthroughConfig.exe"
