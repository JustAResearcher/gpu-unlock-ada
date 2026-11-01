#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import os
import sys
import shutil
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
GPU_ROOT = PROJECT_ROOT.parent

def package_drive(target_path: Path):
    print(f"=== Packaging Universal GPU Unlock to: {target_path} ===")
    efi_boot = target_path / "EFI" / "BOOT"
    efi_vbios = target_path / "EFI" / "VBIOS"
    efi_boot.mkdir(parents=True, exist_ok=True)
    efi_vbios.mkdir(parents=True, exist_ok=True)

    # 1. Copy BOOTX64.EFI
    src_efi = PROJECT_ROOT / "dist" / "EFI" / "BOOT" / "BOOTX64.EFI"
    if not src_efi.exists():
        print("[!] BOOTX64.EFI not found in dist. Run build.py first!")
        sys.exit(1)
    shutil.copy2(src_efi, efi_boot / "BOOTX64.EFI")
    print(f"  [+] Installed BOOTX64.EFI -> {efi_boot / 'BOOTX64.EFI'}")

    # 2. Copy Windows Dumper scripts
    shutil.copy2(PROJECT_ROOT / "dump_vbios.bat", target_path / "dump_vbios.bat")
    shutil.copy2(PROJECT_ROOT / "dump_vbios.ps1", target_path / "dump_vbios.ps1")
    print(f"  [+] Installed dump_vbios.bat & dump_vbios.ps1")

    # 3. Copy nvflashk.exe if available
    nvflashk = GPU_ROOT / "nvflashk.exe"
    if nvflashk.exists():
        shutil.copy2(nvflashk, target_path / "nvflashk.exe")
        print(f"  [+] Installed nvflashk.exe")

    # 4. Check for existing 4070 Ti ROM and copy with strict ID if present
    my_rom = GPU_ROOT / "my_4070ti.rom"
    if my_rom.exists():
        target_rom = efi_vbios / "10de_2782_1458_40e8.rom"
        shutil.copy2(my_rom, target_rom)
        print(f"  [+] Pre-loaded RTX 4070 Ti ROM -> {target_rom}")

    # 4b. Copy universal firmware assets to EFI\ASSETS
    assets_src = PROJECT_ROOT / "UNIVERSAL-ASSETS"
    if not assets_src.exists():
        assets_src = PROJECT_ROOT / "EFI" / "ASSETS"
    if not assets_src.exists():
        assets_src = GPU_ROOT / "UNIVERSAL-ASSETS"
    
    if assets_src.exists():
        assets_dst = target_path / "EFI" / "ASSETS"
        assets_dst.mkdir(parents=True, exist_ok=True)
        for item in assets_src.iterdir():
            if item.is_file():
                shutil.copy2(item, assets_dst / item.name)
        print(f"  [+] Installed universal firmware assets -> {assets_dst}")

    # 5. Instructions
    readme = target_path / "ИНСТРУКЦИЯ.txt"
    readme.write_text(
        "=== ИНСТРУКЦИЯ ПО ИСПОЛЬЗОВАНИЮ UNIVERSAL GPU UNLOCK ===\n\n"
        "1. В Windows запустите 'dump_vbios.bat' от имени Администратора.\n"
        "   Скрипт сам определит все ваши видеокарты (30 и 40 серию) и сохранит их\n"
        "   в папку EFI\\VBIOS\\ строго под их аппаратными идентификаторами.\n\n"
        "   Примечание: если у вас стоят две одинаковые видеокарты (например, две RTX 3060),\n"
        "   им будет достаточно одного файла дампа по их общему Hardware ID!\n\n"
        "2. Перезагрузите ПК и выберите загрузку с этой флешки в Boot Menu (F12 / F11).\n\n"
        "3. Загрузчик автоматически:\n"
        "   - Просканирует шину PCIe и определит все видеокарты.\n"
        "   - Подгрузит их VBIOS файлы в оперативную память (ОЗУ).\n"
        "   - Применит разблокировку производительности в RAM (Zero-Flash, физический чип НЕ трогается).\n"
        "   - Запустит Windows Boot Manager.\n\n"
        "БЕЗОПАСНОСТЬ: Разблокировка на 100% энергозависима (только в RAM).\n"
        "При обычной перезагрузке без флешки все видеокарты остаются в 100% заводском стоке.\n",
        encoding="utf-8"
    )
    print(f"  [+] Created ИНСТРУКЦИЯ.txt")
    print("\n[SUCCESS] Flash drive package is complete and ready to boot!")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    else:
        # Default package target: staging folder in project
        target = PROJECT_ROOT / "usb_release_staging"
    package_drive(target)

