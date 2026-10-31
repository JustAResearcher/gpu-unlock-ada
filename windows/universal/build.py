#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import os
import sys
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT_DIR = ROOT

# Search for w64devkit in parent hierarchy
w64devkit_bin = None
for p in [ROOT, *ROOT.parents]:
    candidate = p / "tools" / "w64devkit" / "bin"
    if candidate.exists() and (candidate / "gcc.exe").exists():
        w64devkit_bin = candidate
        break

if w64devkit_bin:
    GCC = str(w64devkit_bin / "gcc.exe")
    LD = str(w64devkit_bin / "x86_64-w64-mingw32-ld.exe")
else:
    GCC = "gcc"
    LD = "x86_64-w64-mingw32-ld"
EDK2_INC = PROJECT_DIR / "vendor" / "edk2" / "Include"

BUILD_DIR = PROJECT_DIR / "build"
DIST_DIR = PROJECT_DIR / "dist" / "EFI" / "BOOT"
BUILD_DIR.mkdir(parents=True, exist_ok=True)
DIST_DIR.mkdir(parents=True, exist_ok=True)

flags = [
    "-std=c11", "-O2", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-Wno-misleading-indentation",
    "-D__KERNEL__", "-DGU_UEFI_PORT", "-DGU_AD104_TRANSACTION",
    "-DGU_ENTRY_LATCHED_SS1_FULL",
    f"-I{PROJECT_DIR}",
    f"-I{PROJECT_DIR / 'include'}",
    f"-I{PROJECT_DIR / 'compat'}",
    f"-I{EDK2_INC}",
    f"-I{EDK2_INC / 'X64'}",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mno-stack-arg-probe", "-mno-red-zone", "-fshort-wchar", "-fstack-usage"
]

sources = [
    "src/format",
    "src/logger",
    "src/sha256",
    "src/runtime",
    "src/compat",
    "src/port",
    "src/profile",
    "src/checks",
    "src/asset_loader",
    "src/pci_scan",
    "src/vbios_loader",
    "src/vbios_parser",
    "src/display_release",
    "vendor/unlock_610",
    "vendor/prep_610",
    "vendor/layout_610",
    "vendor/vbios_610",
    "vendor/gfw_eval",
    "src/unlock_engine",
    "src/app"
]

objects = []
print("=== COMPILING C MODULES (UNIVERSAL MULTI-GPU UNLOCK ENGINE) ===")
for s in sources:
    obj_name = s.replace('/', '_') + '.o'
    obj_path = BUILD_DIR / obj_name
    src_path = PROJECT_DIR / f"{s}.c"
    cmd = [GCC] + flags + ["-c", str(src_path), "-o", str(obj_path)]
    print(f"  [CC] {s}.c -> {obj_name}")
    res = subprocess.run(cmd, cwd=str(PROJECT_DIR))
    if res.returncode != 0:
        print(f"[!] Compilation error in {s}.c")
        sys.exit(1)
    objects.append(str(obj_path))

efi_out = BUILD_DIR / "BOOTX64.EFI"
print("\n=== LINKING STANDALONE EFI APPLICATION ===")
link_cmd = [
    LD, "-shared", "-nostdlib", "--subsystem", "10",
    "--image-base", "0x180000000", "-e", "UefiMain",
    "-o", str(efi_out)
] + objects

res = subprocess.run(link_cmd, cwd=str(PROJECT_DIR))
if res.returncode != 0:
    print("[!] Linking failed")
    sys.exit(1)

print(f"  [OK] Linked binary: {efi_out} ({efi_out.stat().st_size} bytes)")

# Zero out Data Directory 1 (Imports) to prevent UEFI ImageLoader dependency faults
data = bytearray(efi_out.read_bytes())
pe_off = struct.unpack_from('<I', data, 60)[0]
opt_off = pe_off + 24
imp_rva, imp_size = struct.unpack_from('<II', data, opt_off + 112 + 8)
if imp_size > 0:
    print(f"  [FIX] Zeroing out PE imports directory (rva={hex(imp_rva)}, size={hex(imp_size)})")
    data[opt_off + 112 + 8 : opt_off + 112 + 16] = b'\x00' * 8
    efi_out.write_bytes(data)

# Deploy to dist directory
dist_efi = DIST_DIR / "BOOTX64.EFI"
dist_efi.write_bytes(data)
print(f"\n=== DEPLOYMENT READY ===")
print(f"  Target: {dist_efi}")
import hashlib
print(f"  SHA-256: {hashlib.sha256(data).hexdigest()}")
print("  Build completed successfully!")
