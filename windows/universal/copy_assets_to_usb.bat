@echo off
setlocal enabledelayedexpansion
title Universal GPU Unlock - Setup USB Assets
echo ======================================================================
echo           UNIVERSAL GPU UNLOCK - COPY ASSETS TO USB
echo ======================================================================
echo.

:: 1. Locate source assets directory
set "SRC_DIR="
if exist "%~dp0UNIVERSAL-ASSETS\gsp_ga10x.bin" set "SRC_DIR=%~dp0UNIVERSAL-ASSETS"
if exist "%~dp0EFI\ASSETS\gsp_ga10x.bin" set "SRC_DIR=%~dp0EFI\ASSETS"
if exist "%~dp0ASSETS\gsp_ga10x.bin" set "SRC_DIR=%~dp0ASSETS"
if exist "%~dp0gsp_ga10x.bin" set "SRC_DIR=%~dp0"

if "%SRC_DIR%"=="" (
    echo [!] Could not locate firmware files ^(gsp_ga10x.bin^) automatically.
    echo Please ensure this .bat file is placed in the folder with UNIVERSAL-ASSETS.
    echo.
    pause
    exit /b 1
)

echo [OK] Found firmware assets in: "%SRC_DIR%"
echo.

:: 2. Auto-detect USB drive with BOOTX64.EFI
set "USB_DRIVE="
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%D:\EFI\BOOT\BOOTX64.EFI" (
        set "USB_DRIVE=%%D:"
        echo [OK] Auto-detected USB boot drive at %%D:
        goto :FOUND_USB
    )
)

:PROMPT_USB
echo Please enter the drive letter of your USB flash drive (for example E: or F:):
set /p "USB_DRIVE=Drive letter: "
if "%USB_DRIVE%"=="" goto :PROMPT_USB
if "%USB_DRIVE:~-1%" NEQ ":" set "USB_DRIVE=%USB_DRIVE%:"

if not exist "%USB_DRIVE%\" (
    echo [ERROR] Drive %USB_DRIVE% was not found. Please try again.
    goto :PROMPT_USB
)

:FOUND_USB
echo.
echo Target USB Drive: %USB_DRIVE%
echo Target folder:    %USB_DRIVE%\EFI\ASSETS\
echo.

if not exist "%USB_DRIVE%\EFI\ASSETS" mkdir "%USB_DRIVE%\EFI\ASSETS"

echo Copying firmware files to %USB_DRIVE%\EFI\ASSETS\ ...
xcopy "%SRC_DIR%\*.bin" "%USB_DRIVE%\EFI\ASSETS\" /Y /Q /R >nul

if exist "%SRC_DIR%\NVIDIA-LICENSE" (
    copy /Y "%SRC_DIR%\NVIDIA-LICENSE" "%USB_DRIVE%\EFI\ASSETS\" >nul
)

echo.
echo ======================================================================
if exist "%USB_DRIVE%\EFI\ASSETS\gsp_ga10x.bin" (
    echo [SUCCESS] Firmware assets copied successfully to %USB_DRIVE%\EFI\ASSETS\
    echo.
    echo Verified assets on %USB_DRIVE%:
    dir /b "%USB_DRIVE%\EFI\ASSETS\*.bin"
    echo.
    echo Your USB drive is ready! Reboot and select [U] Unlock.
) else (
    echo [ERROR] Copy failed or files missing!
)
echo ======================================================================
echo.
pause
