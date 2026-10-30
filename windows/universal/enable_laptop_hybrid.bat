@echo off
:: Self-elevate to Administrator if not already running as admin
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [i] Requesting Administrator privileges to configure registry...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"
if exist "%~dp0tools\enable_laptop_hybrid.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\enable_laptop_hybrid.ps1" %*
) else if exist "%~dp0enable_laptop_hybrid.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0enable_laptop_hybrid.ps1" %*
) else (
    echo [ERROR] enable_laptop_hybrid.ps1 not found!
)
echo.
echo Press any key to exit...
pause >nul
