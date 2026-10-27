#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Утилита генерации и верификации пяти микрокодовых файлов AD102 для папки AD104-ASSETS.
Проверяет размеры и SHA-256 хеши в точном соответствии с profile.c.
"""

import sys
import hashlib
import argparse
from pathlib import Path

# Эталонные хеши и размеры из profile.c
PINS = {
    "booter_load_ad102.bin": (
        "6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49", 57672
    ),
    "v67-ad102-610.43.03-four-plm.bin": (
        "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb", 64000
    ),
    "gsp_ga10x.bin": (
        "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a", 84277400
    ),
    "gsp_boot_ad102.bin": (
        "45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0", 36864
    ),
    "gsp_boot_desc_ad102.bin": (
        "83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42", 84
    )
}

def verify_dir(target_dir: Path) -> bool:
    print(f"=== ПРОВЕРКА ФАЙЛОВ ПРОШИВКИ В: {target_dir} ===")
    all_ok = True
    for name, (expected_hash, expected_size) in PINS.items():
        file_path = target_dir / name
        if not file_path.exists():
            print(f"  [X] ОТСУТСТВУЕТ: {name}")
            all_ok = False
            continue
        data = file_path.read_bytes()
        actual_size = len(data)
        actual_hash = hashlib.sha256(data).hexdigest()
        
        size_ok = (actual_size == expected_size)
        hash_ok = (actual_hash == expected_hash)
        status = "OK" if (size_ok and hash_ok) else "ОШИБКА"
        print(f"  [{status}] {name:32} (size={actual_size}, hash_match={hash_ok})")
        if not (size_ok and hash_ok):
            all_ok = False
    return all_ok

def main():
    parser = argparse.ArgumentParser(description="Верификатор и менеджер ассетов AD104-ASSETS")
    parser.add_argument("dir", nargs="?", default="AD104-ASSETS", help="Путь к папке AD104-ASSETS")
    parser.add_argument("--copy-to", help="Скопировать проверенные ассеты в указанную папку (например, на USB G:/AD104-ASSETS)")
    args = parser.parse_args()

    target = Path(args.dir)
    if not target.is_dir():
        # Попробуем найти в корне проекта
        alt = Path(__file__).resolve().parents[3] / args.dir
        if alt.is_dir():
            target = alt
        else:
            print(f"Папка '{args.dir}' не найдена.")
            sys.exit(1)

    ok = verify_dir(target)
    if ok:
        print("\n[V] ВСЕ 5 ФАЙЛОВ ПРОШИВКИ ПОЛНОСТЬЮ СООТВЕТСТВУЮТ ЭТАЛОНУ!")
        if args.copy_to:
            dest = Path(args.copy_to)
            dest.mkdir(parents=True, exist_ok=True)
            import shutil
            for name in PINS:
                src_file = target / name
                dst_file = dest / name
                shutil.copy2(src_file, dst_file)
                print(f"Скопировано: {name} -> {dst_file}")
            print("\nУспешно перенесено на целевой носитель!")
    else:
        print("\n[X] Внимание: Найдены несоответствия или отсутствующие файлы.")
        sys.exit(1)

if __name__ == "__main__":
    main()
