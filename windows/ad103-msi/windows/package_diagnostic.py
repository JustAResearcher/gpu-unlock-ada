#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package the isolated, read-only USB diagnostic with its validation evidence."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from build import verify_pe

ROOT = Path(__file__).resolve().parent.parent
WIN = ROOT / 'windows'
NAME = 'gpu-unlock-windows-usb-diagnostic-0.4.2'
DIAGNOSTIC_SOURCES = {
    'windows/src/' + name + '.c' for name in
    ('app', 'app_io', 'ownership', 'format', 'runtime', 'sha256', 'checks', 'port', 'compat', 'probe', 'capture', 'capture_pe')
} | {'kernel/gpu_unlock/vbios_610.c', 'kernel/gpu_unlock/gfw_eval.c'}
VM_CASES = {'usb_absent_target', 'missing_report_directory', 'non_usb'}
EXPECTED_CHECKS = {'build', 'uefi-host-tests', 'usb-preparation-tests', 'uefi-vm-tests'}
HOST_REQUIRED_SOURCES = {
    'windows/package_diagnostic.py', 'windows/validate_diagnostic.py',
    'windows/Prepare-USB.cmd', 'windows/Prepare-USB.ps1', 'windows/UsbPreparation.psm1',
    'windows/Update-USB.cmd', 'windows/Update-USB.ps1',
    'windows/board_profile.py', 'windows/prepare_assets.py', 'windows/analyze_diagnostic.py',
    'windows/rehearsal_profile.py', 'windows/package_rehearsal.py', 'windows/validate_rehearsal.py',
    'windows/rehearsal/Update-USB.cmd', 'windows/rehearsal/Update-USB.ps1',
    'windows/rehearsal/RehearsalUsb.psm1', 'windows/rehearsal/START-HERE.txt', 'windows/rehearsal/README.md',
    'windows/control-center/BootBackend.psm1', 'windows/control-center/FirmwareVariables.cs',
    'tests/vbios_fixture.h',
    'firmware/fetch_artifacts.py', 'firmware/extract_boot_assets.py', 'firmware/generate_v67.py',
    'firmware/artifacts-610.43.03.json', 'firmware/ada-610.43.03.json', 'firmware/NOTICE.md',
}


def validation_source_hashes(root: Path = ROOT) -> dict[str, str]:
    """Bind host/USB/VM checks to their actual scripts and test dependencies.

    The EFI build record separately binds compiled C and header inputs. Keeping
    all test files in this inventory also detects tests added after validation.
    """
    names = HOST_REQUIRED_SOURCES | {
        path.relative_to(root).as_posix()
        for path in (root / 'windows/tests').rglob('*')
        if path.is_file() and path.suffix in {'.c', '.h', '.py', '.ps1'} and
        '__pycache__' not in path.relative_to(root).parts
    }
    result = {}
    for name in sorted(names):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing or linked validation source: ' + name)
        result[name] = digest(path.read_bytes())
    return result


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_inputs(image: bytes, result: dict, validation: dict, vm: dict,
                 root: Path = ROOT) -> None:
    if (result.get('mode') != 'diagnostic' or result.get('hardware_tested') is not False or
            result.get('board_profile') is not None or result.get('helper_protocol') is not None):
        raise ValueError('Only an unvalidated diagnostic build can be packaged.')
    if result.get('sha256') != digest(image) or result.get('size') != len(image):
        raise ValueError('Diagnostic image does not match its build record.')
    verify_pe(image)
    compiled = result.get('compiled_sources')
    if not isinstance(compiled, list) or len(compiled) != len(DIAGNOSTIC_SOURCES) or set(compiled) != DIAGNOSTIC_SOURCES:
        raise ValueError('Diagnostic must exclude transaction, firmware loader and session sources.')
    hashes = result.get('source_sha256')
    if not isinstance(hashes, dict) or not (DIAGNOSTIC_SOURCES | {'windows/build.py'}) <= hashes.keys():
        raise ValueError('Build provenance is incomplete.')
    for name, expected in hashes.items():
        path = root / name
        if (Path(name).is_absolute() or '..' in Path(name).parts or path.is_symlink() or
                not path.is_file() or digest(path.read_bytes()) != expected):
            raise ValueError('Source changed since the diagnostic build: ' + name)
    # Independent build-inclusion regression checks. These are not a substitute
    # for the code review, write-denial tests, and source manifest above.
    for forbidden in (b'Falcon start', b'unlock_transaction_dma', b'UNLOCK_SUCCESS',
                      b'Press U once', b'booter_load_ad102.bin', b'GUSSN001'):
        if forbidden in image:
            raise ValueError('Unexpected experimental routine/string in diagnostic image.')
    if b'DIAGNOSTIC_ONLY:' not in image:
        raise ValueError('Diagnostic entry-point marker is missing.')
    if (validation.get('all_passed') is not True or validation.get('hardware_tested') is not False or
            validation.get('windows_runtime_tested') is not False or validation.get('image_sha256') != digest(image)):
        raise ValueError('Missing, failed, or mismatched host validation.')
    checks = validation.get('checks')
    if (not isinstance(checks, list) or len(checks) != len(EXPECTED_CHECKS) or
            any(not isinstance(c, dict) or c.get('exit_code') != 0 for c in checks) or
            {c.get('name') for c in checks} != EXPECTED_CHECKS):
        raise ValueError('Every required host/VM check must contain a successful named command result.')
    if validation.get('tested_source_sha256') != validation_source_hashes(root):
        raise ValueError('Host test, preparation, or validation sources changed since validation.')
    if (vm.get('passed') is not True or vm.get('image_sha256') != digest(image) or
            vm.get('hardware_tested') is not False or vm.get('windows_handoff_tested') is not False or
            vm.get('secure_boot_tested') is not False or vm.get('bitlocker_tested') is not False):
        raise ValueError('Missing, failed, or mismatched emulation record.')
    cases = vm.get('cases', [])
    if len(cases) != len(VM_CASES) or {c.get('name') for c in cases} != VM_CASES or any(
            c.get('passed') is not True or c.get('returned_to_firmware') is not True for c in cases):
        raise ValueError('Every refusal/return VM case must pass.')


def build(output: Path, build_directory: Path, validation_path: Path, vm_path: Path) -> dict:
    image_path = build_directory / 'BOOTX64.EFI'
    result_path = build_directory / 'build-result.json'
    result = json.loads(result_path.read_text())
    validation = json.loads(validation_path.read_text())
    vm = json.loads(vm_path.read_text())
    check_inputs(image_path.read_bytes(), result, validation, vm)
    files = {name: WIN / name for name in (
        'Prepare-USB.cmd', 'Prepare-USB.ps1', 'Update-USB.cmd', 'Update-USB.ps1', 'UsbPreparation.psm1', 'START-HERE.txt',
        'USB-DIAGNOSTIC-GUIDE.md', 'THIRD_PARTY_NOTICES.md')}
    files.update({'LICENSE': ROOT / 'LICENSE', 'USB/EFI/BOOT/BOOTX64.EFI': image_path,
                  'build-result.json': result_path, 'VALIDATION.json': validation_path,
                  'VM-VALIDATION.json': vm_path})
    # Include reproducible source and tests, never the withdrawn native filter,
    # other binaries, firmware assets, uploaded reports, or local machine data.
    for folder in ('windows/src', 'windows/deps', 'windows/tests', 'windows/LICENSES', 'kernel/gpu_unlock'):
        for path in sorted((ROOT / folder).rglob('*')):
            if (path.is_file() and path.suffix in {'.c', '.h', '.inc', '.py', '.ps1', '.txt'} and
                    not any(p in {'__pycache__', 'build', '.cache'} for p in path.relative_to(ROOT).parts)):
                files['source/' + path.relative_to(ROOT).as_posix()] = path
    for name in ('build.py', 'board_profile.py', 'prepare_assets.py', 'analyze_diagnostic.py', 'package_diagnostic.py',
                 'rehearsal_profile.py', 'package_rehearsal.py', 'validate_rehearsal.py',
                 'validate_diagnostic.py', 'Prepare-USB.ps1', 'Prepare-USB.cmd', 'Update-USB.ps1', 'Update-USB.cmd',
                 'UsbPreparation.psm1', 'START-HERE.txt', 'USB-DIAGNOSTIC-GUIDE.md',
                 'README.md', 'OWNERSHIP-ANALYSIS.md', 'THIRD_PARTY_NOTICES.md'):
        files['source/windows/' + name] = WIN / name
    for name in ('Update-USB.cmd','Update-USB.ps1','RehearsalUsb.psm1','START-HERE.txt','README.md'):
        files['source/windows/rehearsal/'+name] = WIN/'rehearsal'/name
    files['source/LICENSE'] = ROOT / 'LICENSE'
    # Dependencies referenced by the included diagnostic/session tests. The
    # backend is source for a fake-adapter test, never an activation entry point.
    for name in ('tests/vbios_fixture.h', 'windows/control-center/BootBackend.psm1',
                 'windows/control-center/FirmwareVariables.cs', 'firmware/fetch_artifacts.py',
                 'firmware/extract_boot_assets.py', 'firmware/generate_v67.py',
                 'firmware/artifacts-610.43.03.json', 'firmware/ada-610.43.03.json', 'firmware/NOTICE.md'):
        files['source/' + name] = ROOT / name
    for name, path in files.items():
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing or linked input: ' + name)
        if (path.suffix.lower() in {'.sys', '.bin', '.exe', '.dll'} or '/native/' in name or
                (path.suffix.lower() == '.efi' and name != 'USB/EFI/BOOT/BOOTX64.EFI')):
            raise ValueError('Forbidden executable payload: ' + name)
    payload = {name: path.read_bytes() for name, path in sorted(files.items())}
    payload['SHA256SUMS.json'] = (json.dumps({name: digest(data) for name, data in payload.items()}, indent=2) + '\n').encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in payload.items():
            info = zipfile.ZipInfo(NAME + '/' + name, date_time=(2026, 10, 25, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    return {'path': str(output.resolve()), 'files': len(payload), 'bytes': output.stat().st_size,
            'sha256': digest(output.read_bytes()), 'efi_sha256': result['sha256']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-directory', type=Path, default=WIN / 'build/diagnostic')
    parser.add_argument('--validation', type=Path, required=True)
    parser.add_argument('--vm-validation', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.build_directory, args.validation, args.vm_validation), indent=2))
