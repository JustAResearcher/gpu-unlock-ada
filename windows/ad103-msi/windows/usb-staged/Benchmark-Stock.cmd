@echo off
setlocal
set "GuPowerShell=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "GuPowerShell=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%GuPowerShell%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Benchmark.ps1" -Mode stock
set "GuBenchResult=%ERRORLEVEL%"
pause
exit /b %GuBenchResult%
