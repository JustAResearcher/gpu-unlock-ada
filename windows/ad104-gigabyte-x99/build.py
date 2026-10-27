#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Standalone builder for AD104 RTX 4070 Ti on Huananzhi X99 platform.
Compiles freestanding C11 sources and links AD104-EXPERIMENTAL.EFI.
"""
import sys
import shutil
import struct
import argparse
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD_DIR = HERE / "build"

def find_toolchain():
    # Prefer w64devkit if present in workspace
    ws_tools = HERE.parents[2] / "tools" / "w64devkit" / "bin"
    gcc = ws_tools / "gcc.exe"
    ld = ws_tools / "x86_64-w64-mingw32-ld.exe"
    if gcc.exists() and ld.exists():
        return str(gcc), str(ld)
    
    # Fallback to PATH
    gcc_path = shutil.which("gcc")
    ld_path = shutil.which("x86_64-w64-mingw32-ld") or shutil.which("ld")
    if not gcc_path or not ld_path:
        raise RuntimeError("GCC and LD toolchain not found. Ensure w64devkit or MinGW-w64 is in PATH.")
    return gcc_path, ld_path

def main():
    parser = argparse.ArgumentParser(description="Build AD104-EXPERIMENTAL.EFI for Gigabyte RTX 4070 Ti + X99")
    parser.add_argument("--deploy", help="Deploy built binary to target path (e.g. G:/EFI/BOOT/BOOTX64.EFI)")
    args = parser.parse_args()

    gcc, ld = find_toolchain()
    print(f"Toolchain: gcc={gcc}, ld={ld}")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    flags = [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-misleading-indentation",
        "-D__KERNEL__", "-DGU_UEFI_PORT", "-DGU_AD104_TRANSACTION",
        "-DGU_ENTRY_LATCHED_SS1_FULL",
        "-Icompat", "-Ivendor/edk2/Include", "-Ivendor/edk2/Include/X64",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-mno-stack-arg-probe", "-mno-red-zone", "-fshort-wchar", "-fstack-usage"
    ]

    sources = [
        'app', 'boot_io', 'x99_release', 'port', 'compat', 'checks',
        'profile', 'sha256', 'runtime', 'format',
        'vendor/unlock_610', 'vendor/prep_610', 'vendor/layout_610',
        'vendor/vbios_610', 'vendor/gfw_eval'
    ]

    objects = []
    print("\n=== COMPILING C OBJECTS ===")
    for s in sources:
        obj_name = (s.replace('/', '_')) + '.o'
        obj_path = BUILD_DIR / obj_name
        src_path = HERE / (s + '.c')
        cmd = [gcc] + flags + ["-c", str(src_path), "-o", str(obj_path)]
        print(f"Compiling {s}.c -> {obj_name}...")
        res = subprocess.run(cmd, cwd=str(HERE))
        if res.returncode != 0:
            print(f"Error compiling {s}.c")
            sys.exit(1)
        objects.append(str(obj_path))

    efi_out = BUILD_DIR / "AD104-EXPERIMENTAL.EFI"
    print("\n=== LINKING EFI APPLICATION ===")
    link_cmd = [
        ld, "-shared", "-nostdlib", "--subsystem", "10",
        "--image-base", "0x180000000", "-e", "UefiMain",
        "-o", str(efi_out)
    ] + objects

    res = subprocess.run(link_cmd, cwd=str(HERE))
    if res.returncode != 0:
        print("Link failed")
        sys.exit(1)

    print(f"Linked EFI binary: {efi_out} ({efi_out.stat().st_size} bytes)")

    # Zero out Data Directory 1 (Imports) for strict PE format compliance
    data = bytearray(efi_out.read_bytes())
    pe = struct.unpack_from('<I', data, 60)[0]
    opt = pe + 24
    imp_rva, imp_size = struct.unpack_from('<II', data, opt + 112 + 8)
    print(f"Zeroing imports directory entry: rva={hex(imp_rva)} size={hex(imp_size)}")
    data[opt + 112 + 8 : opt + 112 + 16] = b'\x00' * 8
    efi_out.write_bytes(data)

    print("\n=== RUNNING PE & BINARY VERIFIERS ===")
    sys.path.insert(0, str(HERE / "tools"))
    import pe_format
    pe_format.validate_pe(bytes(data))
    print("[OK] PE Validation passed!")

    res = subprocess.run([sys.executable, str(HERE / "tools" / "verify_experimental.py")], cwd=str(HERE))
    if res.returncode != 0:
        print("verify_experimental.py failed!")
        sys.exit(1)

    import hashlib
    sha = hashlib.sha256(data).hexdigest()
    print(f"\n[SUCCESS] Build complete! SHA-256: {sha}")

    if args.deploy:
        deploy_str = args.deploy
        if len(deploy_str) == 2 and deploy_str[1] == ':':
            deploy_str += '\\'
        target = Path(deploy_str)
        if target.is_dir() or deploy_str.endswith(('\\', '/')):
            target = target / "EFI" / "BOOT" / "BOOTX64.EFI"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        print(f"[DEPLOYED] Successfully copied to: {target}")

if __name__ == "__main__":
    main()
