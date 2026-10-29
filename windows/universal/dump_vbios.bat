@echo off
:: Self-elevate to Administrator if not already running as admin
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [i] Requesting Administrator privileges to access GPU VBIOS...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dump_vbios.ps1"
echo.
echo Press any key to exit...
pause >nul

