@echo off
setlocal EnableDelayedExpansion
title Copy Universal GPU Unlock to USB Flash Drive

echo ====================================================
echo   Copy Universal GPU Unlock to USB Flash Drive
echo ====================================================
echo.

set "DRIVE=%~1"
if "!DRIVE!"=="" (
    set /p "DRIVE=Enter USB drive letter: "
)

if "!DRIVE!"=="" (
    echo.
    echo [ERROR] Drive letter not specified!
    echo.
    pause
    exit /b 1
)

set "DRIVE=!DRIVE:~0,1!:"

if /i "!DRIVE!"=="C:" (
    echo.
    echo [ERROR] Drive C: is the Windows system drive!
    echo.
    pause
    exit /b 1
)

if not exist "!DRIVE!\" (
    echo.
    echo [ERROR] Drive !DRIVE! was not found! Please check connection.
    echo.
    pause
    exit /b 1
)

echo.
echo Target Drive: !DRIVE!\
echo Copying files to !DRIVE!\ ...
echo.

if exist "%~dp0dist\*" (
    echo [1/4] Copying EFI loader...
    xcopy /E /I /Y "%~dp0dist\*" "!DRIVE!\" >nul
)

if not exist "!DRIVE!\EFI\ASSETS" mkdir "!DRIVE!\EFI\ASSETS" >nul 2>&1

if exist "%~dp0UNIVERSAL-ASSETS\*" (
    echo [2/4] Copying firmware assets to \EFI\ASSETS\...
    xcopy /E /I /Y "%~dp0UNIVERSAL-ASSETS\*" "!DRIVE!\EFI\ASSETS\" >nul
)

if exist "%~dp0EFI\*" (
    echo [3/4] Copying VBIOS...
    xcopy /E /I /Y "%~dp0EFI\*" "!DRIVE!\EFI\" >nul
)

echo [4/4] Copying Windows utilities...
if exist "%~dp0dump_vbios.bat" copy /Y "%~dp0dump_vbios.bat" "!DRIVE!\" >nul
if exist "%~dp0dump_vbios.ps1" copy /Y "%~dp0dump_vbios.ps1" "!DRIVE!\" >nul
if exist "%~dp0enable_laptop_hybrid.bat" copy /Y "%~dp0enable_laptop_hybrid.bat" "!DRIVE!\" >nul
if exist "%~dp0enable_laptop_hybrid.ps1" copy /Y "%~dp0enable_laptop_hybrid.ps1" "!DRIVE!\" >nul
if exist "%~dp0nvflashk.exe" (
    copy /Y "%~dp0nvflashk.exe" "!DRIVE!\" >nul
) else if exist "%~dp0..\nvflashk.exe" (
    copy /Y "%~dp0..\nvflashk.exe" "!DRIVE!\" >nul
)
if exist "%~dp0usb_release_staging\ИНСТРУКЦИЯ.txt" copy /Y "%~dp0usb_release_staging\ИНСТРУКЦИЯ.txt" "!DRIVE!\" >nul

echo.
if exist "!DRIVE!\EFI\ASSETS\gsp_ga10x.bin" (
    echo ====================================================
    echo [SUCCESS] Flash drive !DRIVE!\ is ready!
    echo           BOOTX64.EFI and all firmware assets copied.
    echo ====================================================
) else (
    echo ====================================================
    echo [WARNING] gsp_ga10x.bin missing from !DRIVE!\EFI\ASSETS\
    echo ====================================================
)

echo.
if "%~1"=="" pause
