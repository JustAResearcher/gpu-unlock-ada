@echo off
setlocal
set "GuPowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "GuPowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%GuPowerShell%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-USB.ps1"
set "GuResult=%ERRORLEVEL%"
echo.
pause
exit /b %GuResult%
