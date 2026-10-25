#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot an explicitly selected EFI build in QEMU/OVMF without a GPU or host disks.

Requires qemu-system-x86_64, OVMF, mtools and dosfstools. A directory made by
extracting Ubuntu packages can be passed with --tool-root. The output directory
must not exist. Only newly created regular disk-image files are formatted.

This tests EFI loading, report I/O, refusal and return to firmware. It does not
test NVIDIA hardware, Windows boot, BitLocker, Secure Boot, or unlocking.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tools(root: Path | None) -> tuple[dict[str, str], dict[str, str]]:
    env = os.environ.copy()
    if root:
        root = root.resolve()
        lib = [root / 'usr/lib/x86_64-linux-gnu', root / 'lib/x86_64-linux-gnu']
        env['LD_LIBRARY_PATH'] = ':'.join(map(str, lib)) + (
            ':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
        env['QEMU_MODULE_DIR'] = str(root / 'usr/lib/x86_64-linux-gnu/qemu')
        paths = {
            'qemu': root / 'usr/bin/qemu-system-x86_64',
            'mkfs': root / 'usr/sbin/mkfs.fat',
            'mmd': root / 'usr/bin/mmd',
            'mcopy': root / 'usr/bin/mcopy',
            'mdir': root / 'usr/bin/mdir',
            'code': root / 'usr/share/OVMF/OVMF_CODE_4M.fd',
            'vars': root / 'usr/share/OVMF/OVMF_VARS_4M.fd',
            'vga': root / 'usr/share/seabios/vgabios-stdvga.bin',
            'qemu_data': root / 'usr/share/qemu',
        }
    else:
        paths = {name: shutil.which(binary) for name, binary in {
            'qemu': 'qemu-system-x86_64', 'mkfs': 'mkfs.fat',
            'mmd': 'mmd', 'mcopy': 'mcopy', 'mdir': 'mdir',
        }.items()}
        paths.update(code='/usr/share/OVMF/OVMF_CODE_4M.fd',
                     vars='/usr/share/OVMF/OVMF_VARS_4M.fd',
                     vga='/usr/share/seabios/vgabios-stdvga.bin',
                     qemu_data='/usr/share/qemu')
    for name, value in paths.items():
        if not value or not Path(value).exists():
            raise RuntimeError(f'Missing {name}: {value}')
    return {k: str(v) for k, v in paths.items()}, env


def run_case(name: str, output: Path, image: Path, paths: dict[str, str],
             env: dict[str, str], timeout: float, rehearsal: bool = False,
             staged: bool = False) -> dict:
    if rehearsal and staged:
        raise ValueError('Select one image mode')
    folder = output / name
    folder.mkdir()
    disk = folder / 'disk.img'
    with disk.open('xb') as file:
        file.truncate(64 * 1024 * 1024)

    def run(which, *args, check=True):
        return subprocess.run([paths[which], *map(str, args)], env=env,
                              check=check, capture_output=True, text=True)

    run('mkfs', '-F', '32', disk)
    dirs = ['::/EFI', '::/EFI/BOOT']
    if name != 'missing_report_directory':
        dirs.append('::/GPU-UNLOCK-ADA')
    run('mmd', '-i', disk, *dirs)
    run('mcopy', '-i', disk, image, '::/EFI/BOOT/BOOTX64.EFI')
    vars_copy = folder / 'vars.fd'
    shutil.copyfile(paths['vars'], vars_copy)
    command = [paths['qemu'], '-L', paths['qemu_data'], '-machine',
               'q35,accel=tcg', '-m', '256', '-nodefaults',
               '-device', 'VGA,romfile=' + paths['vga'],
               '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=' + paths['code'],
               '-drive', 'if=pflash,format=raw,unit=1,file=' + str(vars_copy)]
    if name == 'non_usb':
        command += ['-drive', 'if=none,id=disk,format=raw,file=' + str(disk),
                    '-device', 'virtio-blk-pci,drive=disk,bootindex=1']
    else:
        command += ['-device', 'qemu-xhci,id=xhci',
                    '-drive', 'if=none,id=usb,format=raw,file=' + str(disk),
                    '-device', 'usb-storage,bus=xhci.0,drive=usb,removable=on,bootindex=1']
    command += ['-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot']
    (folder / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    serial = folder / 'serial.log'
    returned = False
    with serial.open('wb') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log,
                                   stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic() + timeout
            enter_sent = False
            while time.monotonic() < deadline:
                time.sleep(.2)
                text = serial.read_text(errors='replace')
                if 'Press Enter or Esc' in text and not enter_sent:
                    process.stdin.write(b'\r')
                    process.stdin.flush()
                    enter_sent = True
                # This firmware message is emitted only after StartImage returns.
                returned = bool(re.search(r'BdsDxe: failed to start Boot\w+.*: (Unsupported|Not Found|Security Violation)', text))
                if returned or process.poll() is not None:
                    break
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)

    text = serial.read_text(errors='replace')
    finished=('Unlock test finished before driver release. No unlock was attempted.' if staged else
              'Rehearsal finished before driver release.' if rehearsal else 'Diagnostic finished.')
    if not returned or finished not in text:
        raise RuntimeError(f'{name}: EFI application did not return normally; inspect {serial}')
    extracted = folder / 'REPORT.TXT'
    copy = run('mcopy', '-i', disk, '::/GPU-UNLOCK-ADA/RUN00/REPORT.TXT', extracted, check=False)
    if name == 'usb_absent_target':
        if copy.returncode != 0:
            raise RuntimeError(f'{name}: report missing: {copy.stderr}')
        report = extracted.read_text()
        active_mode='staged-unlock-test' if staged else 'firmware-rehearsal'
        expected = ('mode='+active_mode if rehearsal or staged else 'PROFILE_TAG=diagnostic-only mode=diagnostic',
                    'PCI target matches=0 status=0',
                    'FINAL mode='+active_mode+' status=0x8000000000000003 stage=target mutated=0 unlock_attempted=0 app_mmio_writes=0' if rehearsal or staged else
                    'FINAL mode=diagnostic status=0x8000000000000003 stage=target gpu_writes=0')
        if not all(token in report for token in expected):
            raise RuntimeError(f'{name}: unexpected report contents')
        if staged and ('REHEARSAL_INTENT stage=1' in report or 'STAGED_CLAIM_INTENT' in report):
            raise RuntimeError(f'{name}: absent target reached driver release/claim')
    else:
        if copy.returncode == 0 or 'PCI enumeration:' in text:
            raise RuntimeError(f'{name}: rejected storage still reached PCI enumeration/report creation')
        report = None
    listing = run('mdir', '-i', disk, '-/', '::/').stdout
    (folder / 'disk-listing.txt').write_text(listing)
    return {'name': name, 'passed': True, 'returned_to_firmware': True,
            'gpu_present': False, 'report_written': report is not None,
            'report_sha256': sha256(extracted) if report else None,
            'serial_sha256': sha256(serial)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--build-result', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tool-root', type=Path)
    parser.add_argument('--timeout', type=float, default=90)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--rehearsal', action='store_true', help='test refusal/return only for the separate rehearsal image')
    mode.add_argument('--staged', action='store_true', help='test absent-hardware refusal/return only for staged unlock test 0.6.4')
    args = parser.parse_args()
    image = args.image.resolve()
    build = json.loads(args.build_result.read_text())
    expected_mode='staged-unlock-test' if args.staged else 'firmware-rehearsal' if args.rehearsal else 'diagnostic'
    valid_profile=isinstance(build.get('board_profile'),dict) if args.rehearsal or args.staged else build.get('board_profile') is None
    if (build.get('mode') != expected_mode or not valid_profile or
            (args.rehearsal and build.get('unlock_enabled') is not False) or
            (args.staged and (build.get('unlock_enabled') is not True or build.get('version') != '0.6.4-unlock-test')) or
            build.get('helper_protocol') is not None or build.get('sha256') != sha256(image)):
        parser.error('A hash-matched build in the explicitly selected mode is required')
    if args.timeout <= 0 or args.timeout > 300:
        parser.error('timeout must be between 0 and 300 seconds')
    paths, env = tools(args.tool_root)
    if args.rehearsal or args.staged:
        # This build requires an explicit SecureBoot=0, and deliberately refuses
        # a missing variable. Use capable OVMF with an empty, disabled-key store;
        # the plain OVMF build in this tool bundle does not expose that variable.
        capable=Path(paths['code']).with_name('OVMF_CODE_4M.secboot.fd')
        if not capable.is_file():raise RuntimeError('This VM mode needs Secure-Boot-capable OVMF with the empty VARS template')
        paths['code']=str(capable)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {
        'schema_version': 1, 'started_utc': datetime.now(timezone.utc).isoformat(),
        'mode': expected_mode,
        'scope': 'QEMU/OVMF EFI loading, report I/O, refusal and firmware return only',
        'hardware_tested': False, 'windows_handoff_tested': False,
        'secure_boot_tested': False, 'bitlocker_tested': False,
        'unlock_tested': False,
        'image_sha256': sha256(image), 'ovmf_code_sha256': sha256(Path(paths['code'])),
        'ovmf_vars_template_sha256': sha256(Path(paths['vars'])),
        'qemu_version': subprocess.check_output([paths['qemu'], '--version'], env=env, text=True).splitlines()[0],
        'cases': [], 'passed': False,
    }
    try:
        for name in ('usb_absent_target', 'missing_report_directory', 'non_usb'):
            case = run_case(name, output, image, paths, env, args.timeout, args.rehearsal, args.staged)
            result['cases'].append(case)
            print(f'{name}: PASS', flush=True)
        result['passed'] = True
    finally:
        result['finished_utc'] = datetime.now(timezone.utc).isoformat()
        (output / 'vm-validation.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
