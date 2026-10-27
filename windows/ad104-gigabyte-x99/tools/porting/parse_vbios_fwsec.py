#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Утилита анализа дампа VBIOS и извлечения геометрии FWSEC / хешей PROM.
Используется при портировании UEFI-загрузчика на новую видеокарту или ревизию VBIOS.

Что делает скрипт:
1. Автоматически находит начало VBIOS-образа (сигнатура 0x55AA + блок PCIR) даже в полных SPI-дампах.
2. Парсит цепочку блоков расширения PCI ROM (PCIR, NPDS, NPDE) и вычисляет смещение base/ext/adjust.
3. Ищет заголовок BIT ("\xb8\xffBIT\0") и верифицирует контрольную сумму.
4. Находит указатель на ucode table (токен 0x70) и дескриптор FWSEC (токен 0x85).
5. Считывает точные параметры геометрии Falcon/SEC2.
6. Вычисляет размер чистого VBIOS Slot 0 (626 176 байт) и 1 МБ PROM-апертуры MMIO.
7. Генерирует готовый C-код для profile.c.
"""

import sys
import struct
import hashlib
import argparse
from pathlib import Path

def r16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]

def r32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]

def range_check(length: int, off: int, bytes_count: int) -> bool:
    return off > length or bytes_count > length - off

def rom_sig_valid(sig: int) -> bool:
    return sig in (0xaa55, 0x4e56, 0xbb77)

def data_sig_valid(sig: int) -> bool:
    return sig in (0x52494350, 0x5344504e, 0x53494752)  # 'PCIR', 'NPDS', 'RGIS'

def find_vbios_start(rom: bytes) -> int:
    # Проверяем прямое начало
    if len(rom) >= 0x20 and rom_sig_valid(r16(rom, 0)):
        pcir = r16(rom, 0x18)
        if pcir + 4 <= len(rom) and data_sig_valid(r32(rom, pcir)):
            return 0
    # Ищем по границам 512 байт в первых 256 КБ дампа
    limit = min(len(rom), 0x40000)
    for off in range(0, limit, 512):
        if rom_sig_valid(r16(rom, off)):
            if off + 0x1a <= len(rom):
                pcir = off + r16(rom, off + 0x18)
                if pcir + 4 <= len(rom) and data_sig_valid(r32(rom, pcir)):
                    return off
    return -1

def parse_rom_chain(rom: bytes, length: int):
    pos = 0
    base = 0
    ext = 0
    images = 0
    last_end = 0

    if range_check(length, 0, 2) or not rom_sig_valid(r16(rom, 0)):
        raise ValueError("Ошибка: Сигнатура ROM (0x55AA) не найдена.")

    while True:
        images += 1
        if images > 8 or range_check(length, pos, 0x1a) or not rom_sig_valid(r16(rom, pos)):
            break

        pcir = pos + r16(rom, pos + 0x18)
        if range_check(length, pcir, 0x16):
            raise ValueError("Ошибка: PCIR заголовок выходит за границы файла.")

        sig = r32(rom, pcir)
        if not data_sig_valid(sig):
            raise ValueError(f"Неизвестная сигнатура данных: {hex(sig)}")

        bytes_len = r16(rom, pcir + 0x10) * 512
        last = rom[pcir + 0x15] & 0x80
        image_type = rom[pcir + 0x14]
        npde = (pcir + r16(rom, pcir + 0x0a) + 15) & ~15

        if not range_check(length, npde, 4) and r32(rom, npde) == 0x4544504e:  # 'NPDE'
            ext_rev = r16(rom, npde + 4)
            ext_len = r16(rom, npde + 6)
            if ext_rev in (0x100, 0x101):
                sub = r16(rom, npde + 8) * 512
                if ext_len > 10:
                    last = rom[npde + 10] & 0x80
                elif sub < bytes_len:
                    last = 0
                bytes_len = sub

        print(f"  [ROM Chain] pos={hex(pos)} bytes={bytes_len} type={hex(image_type)} last={bool(last)}")
        if not bytes_len or range_check(length, pos, bytes_len):
            raise ValueError("Некорректная длина образа в цепочке ROM.")

        if not base and image_type == 0:
            base = bytes_len
        if not ext and image_type == 0xe0:
            ext = pos

        last_end = pos + bytes_len
        if last:
            break
        pos += bytes_len

    adjust = (ext - base) if (ext and base and ext >= base) else 0
    return adjust, last_end

def parse_descriptor(rom: bytes, length: int, off: int) -> dict:
    if range_check(length, off, 44):
        raise ValueError("Смещение дескриптора FWSEC выходит за границы.")
    hdr = r32(rom, off)
    size = hdr >> 16
    if not (hdr & 1) or ((hdr >> 8) & 0xff) != 3 or size < 44:
        raise ValueError(f"Неподдерживаемый заголовок дескриптора: {hex(hdr)}")
    if range_check(length, off, size):
        raise ValueError("Размер дескриптора выходит за границы.")

    stored = r32(rom, off + 4)
    if not stored:
        raise ValueError("Некорректный размер хранимых данных.")

    image_size = (stored + 255) & ~255
    image_off = off + size
    pkc_off = r32(rom, off + 8)
    interface_off = r32(rom, off + 12)
    imem_pa = r32(rom, off + 16)
    imem_size = r32(rom, off + 20)
    imem_va = r32(rom, off + 24)
    dmem_pa = r32(rom, off + 28)
    dmem_size = r32(rom, off + 32)
    data_off = imem_size
    engine_mask = r16(rom, off + 36)
    ucode_id = rom[off + 38]
    sig_count = rom[off + 39]
    sig_versions = r16(rom, off + 40)
    sig_off = off + 44
    sig_bytes = size - 44

    return {
        "image_off": image_off,
        "image_size": image_size,
        "imem_pa": imem_pa,
        "imem_size": imem_size,
        "imem_va": imem_va,
        "dmem_pa": dmem_pa,
        "dmem_size": dmem_size,
        "data_off": data_off,
        "pkc_off": pkc_off,
        "interface_off": interface_off,
        "ucode_id": ucode_id,
        "engine_mask": engine_mask,
        "sig_count": sig_count,
        "sig_versions": sig_versions,
        "sig_off": sig_off,
        "sig_bytes": sig_bytes
    }

def find_fwsec(rom: bytes, length: int, adjust: int):
    for bit in range(length - 12):
        if r16(rom, bit) != 0xb8ff or r32(rom, bit + 2) != 0x00544942:  # 'BIT\0'
            continue
        size = rom[bit + 8]
        stride = rom[bit + 9]
        tokens = rom[bit + 10]
        if size < 12 or stride < 6 or range_check(length, bit, size):
            continue
        if sum(rom[bit : bit + size]) & 0xff != 0:
            continue

        print(f"  [BIT Header] Найдена сигнатура BIT по смещению {hex(bit)} (size={size}, stride={stride}, tokens={tokens})")
        if range_check(length, bit + size, tokens * stride):
            continue

        for i in range(tokens):
            token_off = bit + size + i * stride
            token = rom[token_off : token_off + stride]
            if token[0] != 0x70 or token[1] != 2 or r16(token, 2) < 4:
                continue
            ptr = r32(token, 4) if stride >= 8 else r16(token, 4)
            if range_check(length, ptr, r16(token, 2)):
                continue
            table = adjust + r32(rom, ptr)
            if range_check(length, table, 6):
                continue
            if rom[table] != 1 or rom[table + 1] < 6 or rom[table + 2] < 6:
                continue

            hdr_len = rom[table + 1]
            entry_stride = rom[table + 2]
            entry_count = rom[table + 3]
            print(f"  [UCode Table] Найдена таблица микрокодов по {hex(table)} (entries={entry_count})")
            for j in range(entry_count):
                entry_off = table + hdr_len + j * entry_stride
                entry = rom[entry_off : entry_off + entry_stride]
                if entry[0] == 0x85:  # FWSEC Descriptor Entry
                    desc_off = adjust + r32(entry, 2)
                    print(f"  [FWSEC Entry] Дескриптор 0x85 найден по смещению {hex(desc_off)}")
                    return parse_descriptor(rom, length, desc_off)
    return None

def main():
    parser = argparse.ArgumentParser(description="Парсер VBIOS для портирования AD104 GPU Unlock")
    parser.add_argument("rom", nargs="?", default="my_4070ti.rom", help="Путь к дампу VBIOS (.rom)")
    args = parser.parse_args()

    rom_path = Path(args.rom)
    if not rom_path.is_file():
        alt = Path(__file__).resolve().parents[3] / args.rom
        if alt.is_file():
            rom_path = alt
        else:
            print(f"Ошибка: Файл '{args.rom}' не найден.")
            sys.exit(1)

    print(f"=== АНАЛИЗ VBIOS: {rom_path} ===")
    raw_rom = rom_path.read_bytes()
    raw_len = len(raw_rom)
    print(f"Размер исходного файла дампа: {raw_len} байт ({hex(raw_len)})")

    # Ищем начало VBIOS
    start_off = find_vbios_start(raw_rom)
    if start_off < 0:
        print("Ошибка: В файле не обнаружена сигнатура VBIOS (0x55AA + PCIR).")
        sys.exit(1)

    if start_off > 0:
        print(f"VBIOS Slot 0 обнаружен со смещения: {hex(start_off)} (пропуск flash-дескриптора)")
    else:
        print("VBIOS начинается с нулевого смещения (0x0).")

    vbios = raw_rom[start_off:]
    vbios_len = len(vbios)

    # Разбираем цепочку ROM
    adjust, vbios_payload_len = parse_rom_chain(vbios, vbios_len)
    print(f"ROM adjust: {hex(adjust)}")
    print(f"Точный размер VBIOS Slot 0: {vbios_payload_len} байт ({hex(vbios_payload_len)})")

    # Вычисляем хеши
    vbios_payload = vbios[:vbios_payload_len]
    vbios_hash = hashlib.sha256(vbios_payload).hexdigest()
    print(f"SHA-256 чистого VBIOS Slot 0: {vbios_hash}")

    # 1 МБ PROM апертура MMIO (BAR0 + 0x300000) дополнена 0xFF за пределами Slot 0
    padded_1mb = bytearray(vbios_payload)
    if len(padded_1mb) < 1048576:
        padded_1mb.extend(b"\xff" * (1048576 - len(padded_1mb)))
    else:
        padded_1mb = padded_1mb[:1048576]
    padded_hash = hashlib.sha256(padded_1mb).hexdigest()
    print(f"SHA-256 1MB MMIO PROM (0xFF padded): {padded_hash}")

    # Ищем геометрию FWSEC
    fwsec = find_fwsec(vbios, vbios_len, adjust)
    if not fwsec:
        print("\n[!] Дескриптор FWSEC не найден в образе.")
        sys.exit(1)

    print("\n=== ГЕОМЕТРИЯ FWSEC ===")
    for k, v in fwsec.items():
        if isinstance(v, int):
            print(f"  {k:15}: {hex(v)} ({v})")
        else:
            print(f"  {k:15}: {v}")

    print("\n" + "=" * 60)
    print("ГОТОВЫЙ КОД ДЛЯ profile.c:")
    print("=" * 60)
    print(f"""BOOLEAN GuAd104PromMatches(const VOID *Data, UINTN Bytes) {{
  if (!Data || Bytes < {vbios_payload_len}) return FALSE;
  /* Чистый VBIOS payload: */
  if (GuHashMatches(Data, {vbios_payload_len}, "{vbios_hash}")) return TRUE;
  /* 1 МБ PROM апертура MMIO (BAR0 + 0x300000): */
  if (Bytes == 1048576) {{
    if (GuHashMatches(Data, Bytes, "{padded_hash}")) return TRUE;
  }}
  return FALSE;
}}

BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *f) {{
  return f && f->image_off=={hex(fwsec['image_off'])} && f->image_size=={hex(fwsec['image_size'])} &&
    f->imem_pa=={hex(fwsec['imem_pa'])} && f->imem_size=={hex(fwsec['imem_size'])} && f->imem_va=={hex(fwsec['imem_va'])} &&
    f->dmem_pa=={hex(fwsec['dmem_pa'])} && f->data_off=={hex(fwsec['data_off'])} && f->dmem_size=={hex(fwsec['dmem_size'])} &&
    f->pkc_off=={hex(fwsec['pkc_off'])} && f->interface_off=={hex(fwsec['interface_off'])} && f->ucode_id=={fwsec['ucode_id']} &&
    f->engine_mask=={hex(fwsec['engine_mask'])} && f->sig_count=={fwsec['sig_count']} && f->sig_versions=={fwsec['sig_versions']} &&
    f->sig_off=={hex(fwsec['sig_off'])} && f->sig_bytes=={fwsec['sig_bytes']};
}}""")
    print("=" * 60)

if __name__ == "__main__":
    main()
