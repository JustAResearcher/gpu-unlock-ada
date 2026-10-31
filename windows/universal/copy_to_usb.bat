@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
title Копирование Universal GPU Unlock на флешку

echo ====================================================
echo   Копирование Universal GPU Unlock на USB-флешку (FAT32)
echo ====================================================
echo.

set "DRIVE=%~1"
if "!DRIVE!"=="" (
    if exist "G:\" (
        set "DEF=G"
    ) else (
        set "DEF="
    )
    if defined DEF (
        set /p "DRIVE=Введите букву вашей флешки [Enter для !DEF!]: "
        if "!DRIVE!"=="" set "DRIVE=!DEF!"
    ) else (
        set /p "DRIVE=Введите букву вашей флешки (например G): "
    )
)

if "!DRIVE!"=="" (
    echo.
    echo [ОШИБКА] Буква диска не указана!
    echo.
    pause
    exit /b 1
)

set "DRIVE=!DRIVE:~0,1!:"

if not exist "!DRIVE!\" (
    echo.
    echo [ОШИБКА] Диск !DRIVE! не найден! Проверьте подключение флешки.
    echo.
    pause
    exit /b 1
)

echo.
echo Копирование файлов на !DRIVE!\ ...

if exist "%~dp0dist\*" xcopy /E /I /Y "%~dp0dist\*" "!DRIVE!\" >nul
if not exist "!DRIVE!\EFI\ASSETS" mkdir "!DRIVE!\EFI\ASSETS" >nul 2>&1
if exist "%~dp0UNIVERSAL-ASSETS\*" xcopy /E /I /Y "%~dp0UNIVERSAL-ASSETS\*" "!DRIVE!\EFI\ASSETS\" >nul
if exist "%~dp0EFI\*" xcopy /E /I /Y "%~dp0EFI\*" "!DRIVE!\EFI\" >nul
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

if errorlevel 1 (
    echo.
    echo [ОШИБКА] Не удалось скопировать файлы. Возможно, диск защищен от записи.
) else (
    echo.
    echo ====================================================
    echo [УСПЕХ] Все файлы успешно записаны на !DRIVE!\!
    echo ====================================================
)

echo.
if "%~1"=="" pause
