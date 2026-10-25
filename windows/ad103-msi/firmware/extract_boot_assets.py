#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the Booter and GSP boot assets from the pinned 610 generated source."""
from __future__ import annotations

import argparse
import hashlib
import json
import zlib
import re
import struct
import sys
from pathlib import Path

MANIFEST = Path(__file__).with_name("artifacts-610.43.03.json")
ADA_MANIFEST = Path(__file__).with_name("ada-610.43.03.json")


MAGIC = 0x314C5442
LABELS = (
    "IMAGE_PROD",
    "HEADER_PROD",
    "SIG_PROD",
    "PATCH_LOC",
    "PATCH_META",
    "NUM_SIGS",
)


def _array_bytes(text: str, label: str) -> tuple[bytes, bool, int]:
    marker = f'BINDATA_LABEL_{label}_data[]'
    if text.count(marker) != 1:
        raise ValueError(f"missing or duplicate {label}")
    start = text.index(marker)
    window = text[max(0, start - 400):start]
    compression = re.findall(r"COMPRESSION: (YES|NO)", window)
    sizes = re.findall(r"DATA SIZE \(bytes\): (\d+)", window)
    if not compression or not sizes:
        raise ValueError(f"missing storage declaration for {label}")
    expected = int(sizes[-1]); compressed = compression[-1] == 'YES'
    if not 0 < expected <= 16 * 1024 * 1024:
        raise ValueError('asset exceeds extraction limit')
    declaration = re.match(re.escape(marker) + r"\s*=\s*\{([^}]*)\};", text[start:], re.S)
    if not declaration:
        raise ValueError('malformed byte-array declaration')
    tokens = declaration[1].strip().rstrip(',').split(',')
    if any(not re.fullmatch(r"\s*0x[0-9a-fA-F]{2}\s*", x) for x in tokens):
        raise ValueError('non-byte token in generated array')
    raw = bytes(int(item, 16) for item in tokens)
    if compressed:
        stored = re.findall(r"COMPRESSED SIZE \(bytes\): (\d+)", window)
        if not stored or len(raw) != int(stored[-1]):
            raise ValueError('compressed storage size mismatch')
        inflater = zlib.decompressobj(-15)
        data = inflater.decompress(raw, expected + 1)
        # NVIDIA BINDATA stores raw DEFLATE followed by the gzip CRC32/ISIZE
        # trailer (the gzip header is omitted). Validate both trailer fields.
        if not inflater.eof or len(inflater.unused_data) != 8 or inflater.unconsumed_tail:
            raise ValueError('truncated, trailing or oversized compressed stream')
        crc, size = struct.unpack('<II', inflater.unused_data)
        if size != expected or crc != zlib.crc32(data):
            raise ValueError('compressed checksum or size mismatch')
    else:
        data = raw
    if len(data) != expected:
        raise ValueError(f"{label}: inflated {len(data)} bytes, header says {expected}")
    return data, compressed, expected


def build_bundle(source_text: str) -> bytes:
    image, _, _ = _array_bytes(source_text, "IMAGE_PROD")
    header, _, _ = _array_bytes(source_text, "HEADER_PROD")
    signatures, _, _ = _array_bytes(source_text, "SIG_PROD")
    patch_loc, _, _ = _array_bytes(source_text, "PATCH_LOC")
    patch_meta, _, _ = _array_bytes(source_text, "PATCH_META")
    num_sigs, _, _ = _array_bytes(source_text, "NUM_SIGS")
    if len(header) != 36 or len(patch_loc) != 4 or len(patch_meta) != 12 or len(num_sigs) != 4:
        raise ValueError("unexpected BooterLoad record size")
    fields = struct.unpack("<9I", header)
    loc = struct.unpack("<I", patch_loc)[0]
    _fuse, engine, ucode = struct.unpack("<3I", patch_meta)
    count = struct.unpack("<I", num_sigs)[0]
    if not count or len(signatures) != count * 384 or fields[4] != 1 or not ucode:
        raise ValueError('unsupported production Booter signature/application table')
    if loc < fields[2] or loc + 384 > fields[2] + fields[3] or fields[2] + fields[3] > len(image):
        raise ValueError('signature patch is outside DMEM image')
    for offset, size in ((fields[0],fields[1]), (fields[2],fields[3]), (fields[5],fields[6])):
        if offset + size > len(image) or offset % 256 or size % 256:
            raise ValueError('invalid Falcon transfer range')
    hdr = struct.pack(
        "<14I",
        fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
        fields[7], fields[8], loc, ucode, engine, count, len(signatures),
    )
    prefix = struct.pack("<II", MAGIC, 1) + hdr + struct.pack("<II", len(image), len(signatures))
    return prefix + image + signatures


PINNED = {
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_GA102.c': '04cd3db7f670adc4efd79d2e76217bb809fffddfbd6f1af884cde2af26dd39af',
    'g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c': '35e5aa9075ad3898b6022ffd5820c953d809caa7dabd530cfc1a83dc2a67c5bd',
}
ADA_PINNED = {
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.c': '0504f640bc36d03c3b617b1d80cf6eee1c8022cdfffb52ed22405aaa316a1460',
    'g_bindata_kgspGetBinArchiveGspRmBoot_AD102.c': 'da110719ed6b680f26f3f7158f6731d1ff20fa859857238e23ac4935bd4cd91c',
}


def extract_assets(source_root: Path, output: Path, chip: str = "ga102") -> None:
    if chip not in ("ga102", "ad102"):
        raise ValueError("unsupported firmware profile: " + chip)
    source_pins = PINNED if chip == "ga102" else ADA_PINNED
    inputs = {}
    for name, digest in source_pins.items():
        data = (source_root / 'src/nvidia/generated' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('pinned generated source mismatch: ' + name)
        inputs[name] = data.decode('ascii')
    booter = build_bundle(inputs[f'g_bindata_kgspGetBinArchiveBooterLoadUcode_{chip.upper()}.c'])
    boot = inputs[f'g_bindata_kgspGetBinArchiveGspRmBoot_{chip.upper()}.c']
    image = _array_bytes(boot, 'UCODE_IMAGE_PROD')[0]
    desc = _array_bytes(boot, 'UCODE_DESC_PROD')[0]
    if len(desc) != 84:
        raise ValueError('unexpected pinned bootloader descriptor length')
    fields = struct.unpack('<21I', desc)
    if fields[0] != 5 or fields[14] != 1 or fields[19] != len(image) or fields[20] != 0:
        raise ValueError('unsupported bootloader descriptor')
    for index in (1,3,8,10,12):
        if not fields[index+1] or fields[index]+fields[index+1] > len(image):
            raise ValueError('bootloader descriptor range outside image')
    blobs = {f'booter_load_{chip}.bin': booter, f'gsp_boot_{chip}.bin': image, f'gsp_boot_desc_{chip}.bin': desc}
    manifest = MANIFEST if chip == "ga102" else ADA_MANIFEST
    pinned = json.loads(manifest.read_text())['derived_assets']
    for name, data in blobs.items():
        if hashlib.sha256(data).hexdigest() != pinned[name]:
            raise ValueError('derived asset differs from its pinned SHA-256: ' + name)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in blobs.items():
        (output/name).write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help='pinned NVIDIA source root')
    parser.add_argument("output", type=Path)
    parser.add_argument("--chip", choices=("ga102", "ad102"), default="ga102",
                        help="ad102 extracts stock research assets only; it does not create an Ada unlock payload")
    args = parser.parse_args()
    extract_assets(args.source, args.output, args.chip)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, zlib.error) as error:
        sys.exit(f"extract failed: {error}")
