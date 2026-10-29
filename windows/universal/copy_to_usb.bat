@echo off
chcp 65001 >nul
title Копирование Universal GPU Unlock на флешку
echo ====================================================
echo   Копирование Universal GPU Unlock на USB-флешку (FAT32)
echo ====================================================
echo.

set /p DRIVE="Введите букву вашей флешки (например G): "
set DRIVE=%DRIVE:~0,1%:

if not exist "%DRIVE%\" (
    echo.
    echo [ОШИБКА] Диск %DRIVE% не найден! Проверьте подключение флешки.
    echo.
    pause
    exit /b 1
)

echo.
echo Копирование универсального загрузчика и ассетов (EFI\BOOT, EFI\ASSETS, EFI\VBIOS) на диск %DRIVE%\ ...
if exist "%~dp0dist\*" xcopy /E /I /Y "%~dp0dist\*" "%DRIVE%\"
if not exist "%DRIVE%\EFI\ASSETS" mkdir "%DRIVE%\EFI\ASSETS"
if exist "%~dp0UNIVERSAL-ASSETS\*" xcopy /E /I /Y "%~dp0UNIVERSAL-ASSETS\*" "%DRIVE%\EFI\ASSETS\"
if exist "%~dp0EFI\*" xcopy /E /I /Y "%~dp0EFI\*" "%DRIVE%\EFI\"
if exist "%~dp0dump_vbios.bat" copy /Y "%~dp0dump_vbios.bat" "%DRIVE%\"
if exist "%~dp0dump_vbios.ps1" copy /Y "%~dp0dump_vbios.ps1" "%DRIVE%\"
if exist "%~dp0nvflashk.exe" (
    copy /Y "%~dp0nvflashk.exe" "%DRIVE%\"
) else if exist "%~dp0..\nvflashk.exe" (
    copy /Y "%~dp0..\nvflashk.exe" "%DRIVE%\"
)
if exist "%~dp0usb_release_staging\ИНСТРУКЦИЯ.txt" copy /Y "%~dp0usb_release_staging\ИНСТРУКЦИЯ.txt" "%DRIVE%\"

if errorlevel 1 (
    echo.
    echo [ОШИБКА] Не удалось скопировать файлы. Возможно, диск защищен от записи.
) else (
    echo.
    echo ====================================================
    echo [УСПЕХ] Все файлы успешно записаны на %DRIVE%\!
    echo ====================================================
)

echo.
pause
