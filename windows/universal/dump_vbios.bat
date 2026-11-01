@echo off
setlocal EnableDelayedExpansion
:: Self-elevate to Administrator if not already running as admin
net session >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [i] Requesting Administrator privileges to access GPU VBIOS...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

:: If smart PowerShell dumper with strict hardware ID matching is present, execute it
if exist "%~dp0dump_vbios.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dump_vbios.ps1"
    goto :done
)

:: Pure batch fallback with multi-GPU index scanning
echo ========================================================
echo   UNIVERSAL VBIOS DUMP (Multi-GPU nvflashk Fallback)
echo ========================================================

set "NVFLASH=%~dp0nvflashk.exe"
if not exist "!NVFLASH!" set "NVFLASH=%~dp0..\nvflashk.exe"

if not exist "!NVFLASH!" (
    echo [ERROR] nvflashk.exe not found!
    echo Please ensure nvflashk.exe is in the same folder.
    goto :done
)

if not exist "%~dp0EFI\VBIOS" mkdir "%~dp0EFI\VBIOS"

for %%I in (0 1 2 3) do (
    echo [i] Attempting VBIOS dump from adapter index %%I...
    "!NVFLASH!" --index=%%I --save "%~dp0EFI\VBIOS\gpu_adapter_%%I.rom" >nul 2>&1
    if not exist "%~dp0EFI\VBIOS\gpu_adapter_%%I.rom" (
        "!NVFLASH!" --index=%%I -b "%~dp0EFI\VBIOS\gpu_adapter_%%I.rom" >nul 2>&1
    )
    if exist "%~dp0EFI\VBIOS\gpu_adapter_%%I.rom" (
        echo [SUCCESS] Adapter %%I saved to EFI\VBIOS\gpu_adapter_%%I.rom
    )
)

:done
echo.
echo Press any key to exit...
pause >nul
