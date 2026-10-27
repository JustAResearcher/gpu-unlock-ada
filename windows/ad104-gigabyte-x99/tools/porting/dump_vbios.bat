@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo =======================================================================
echo   Дамп VBIOS видеокарты NVIDIA Ada Lovelace и анализ геометрии FWSEC
echo =======================================================================
echo.

set OUTPUT_ROM=vbios_dump.rom
set NVFLASH_EXE=

:: 1. Поиск в текущей папке tools\porting
if exist "%~dp0nvflashk.exe" set NVFLASH_EXE=%~dp0nvflashk.exe
if not defined NVFLASH_EXE if exist "%~dp0nvflash.exe" set NVFLASH_EXE=%~dp0nvflash.exe

:: 2. Поиск в корне рабочей папки (GPU Unlock)
if not defined NVFLASH_EXE if exist "..\..\..\nvflashk.exe" set NVFLASH_EXE=..\..\..\nvflashk.exe
if not defined NVFLASH_EXE if exist "..\..\..\nvflash.exe" set NVFLASH_EXE=..\..\..\nvflash.exe

:: 3. Поиск в системном PATH
if not defined NVFLASH_EXE (
    for %%X in (nvflashk.exe nvflash.exe) do (
        if not defined NVFLASH_EXE (
            set NVFLASH_EXE=%%~$PATH:X
        )
    )
)

if not defined NVFLASH_EXE (
    echo [!] Утилита nvflashk.exe / nvflash.exe не обнаружена.
    echo Пожалуйста, поместите бинарник nvflashk.exe в эту папку:
    echo   %~dp0
    echo или в корень проекта. Скачать nvflashk можно с:
    echo   https://github.com/Kefis/nvflashk/releases
    echo.
    goto check_existing
)

echo [OK] Найдена утилита: "%NVFLASH_EXE%"
echo.
echo [1/2] Запуск снятия дампа EEPROM (режим безопасного чтения)...
echo Требуются права Администратора при запросе UAC!
echo.

"%NVFLASH_EXE%" --save "%OUTPUT_ROM%"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [!] Команда --save вернула код %ERRORLEVEL%. Попытка с флагом -b...
    "%NVFLASH_EXE%" -b "%OUTPUT_ROM%"
)

if not exist "%OUTPUT_ROM%" (
    echo.
    echo [X] Ошибка: Не удалось создать файл дампа %OUTPUT_ROM%.
    goto check_existing
)

echo.
echo [V] Дамп успешно сохранен в: %OUTPUT_ROM%
echo.

:check_existing
echo [2/2] Анализ смещений и хешей через parse_vbios_fwsec.py...
echo.

if exist "%OUTPUT_ROM%" (
    python parse_vbios_fwsec.py "%OUTPUT_ROM%"
) else if exist "..\..\..\my_4070ti.rom" (
    echo Анализ существующего дампа my_4070ti.rom:
    python parse_vbios_fwsec.py "..\..\..\my_4070ti.rom"
) else (
    echo [!] Файл дампа не найден для автоматического анализа.
    echo Использование: python parse_vbios_fwsec.py <путь_к_файлу.rom>
)

echo.
echo =======================================================================
echo Готово. Нажмите любую клавишу для выхода.
pause
