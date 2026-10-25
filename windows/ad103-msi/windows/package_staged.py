#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package a validated staged unlock experiment, without NVIDIA asset binaries."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from build import verify_pe
from build_staged import ROOT, WIN, MODE, VERSION, DEFINES, SOURCES, REQUIRED_ASSETS, build_source_hashes
from board_profile import header as board_header
from staged_profile import staged_header, validate_staged_evidence
from validate_staged import validation_source_hashes, EXPECTED_CHECKS, BENCHMARK_FILES

NAME = 'gpu-unlock-windows-usb-unlock-test-0.6.4'
CASES = {'usb_absent_target', 'missing_report_directory', 'non_usb'}
USB_FILES = ('Update-USB.cmd','Update-USB.ps1','StagedUsb.psm1','START-HERE.txt',
             'Collect-WindowsResult.ps1','Benchmark-Stock.cmd','Benchmark-Unlocked.cmd',
             'Compare-Benchmarks.cmd','Run-Benchmark.ps1')

def digest(data):
    return hashlib.sha256(data).hexdigest()

def check_inputs(image, result, validation, vm, generated, root=ROOT):
    if (result.get('mode') != MODE or result.get('version') != VERSION or
        result.get('hardware_tested') is not False or result.get('unlock_enabled') is not True or
        result.get('staged_unlock_enabled') is not True or result.get('helper_protocol') is not None or
        result.get('build_defines') != DEFINES or result.get('assets_verified') is not True or
        result.get('required_assets') != REQUIRED_ASSETS):
        raise ValueError('Only the explicit staged unlock test with verified assets can be packaged')
    if result.get('sha256') != digest(image) or result.get('size') != len(image):
        raise ValueError('EFI/build mismatch')
    verify_pe(image)
    compiled = result.get('compiled_sources', [])
    if set(compiled) != SOURCES or len(compiled) != len(SOURCES):
        raise ValueError('Unexpected compiled source inventory')
    for marker in (b'0.6.4-unlock-test', b'mode=staged-unlock-test', b'STAGED_UNLOCK_INTENT:',
                   b'UNLOCK_VERIFIED:', b'booter_load_ad102.bin'):
        if marker not in image: raise ValueError('Staged entry/payload marker absent')
    for marker in (b'DIAGNOSTIC_ONLY:', b'REHEARSAL_ONLY:', b'GUSSN001', b'EXPERIMENTAL WINDOWS PORT'):
        if marker in image: raise ValueError('Unexpected legacy or alternate entry point')
    capture = result.get('capture_profile')
    if (not isinstance(capture, dict) or capture.get('unlock_authorized') is not False or
        capture.get('board') != result.get('board_profile')):
        raise ValueError('Invalid staged capture profile')
    validate_staged_evidence(capture)
    expected = {'board_profile.h': board_header(capture['board']).encode('ascii'),
                'rehearsal_profile.h': staged_header(capture).encode('ascii')}
    if (generated != expected or
        result.get('generated_header_sha256') != {name:digest(data) for name,data in expected.items()}):
        raise ValueError('Generated profile headers do not match validated evidence')
    if result.get('source_sha256') != build_source_hashes(root):
        raise ValueError('Build source inventory changed or is incomplete')
    if (validation.get('all_passed') is not True or validation.get('mode') != MODE or
        validation.get('hardware_tested') is not False or validation.get('windows_runtime_tested') is not False or
        validation.get('real_transaction_verified') is not True or validation.get('real_asset_io_verified') is not True or
        validation.get('assets_verified') is not True or
        validation.get('required_assets') != REQUIRED_ASSETS or validation.get('image_sha256') != digest(image) or
        validation.get('tested_source_sha256') != validation_source_hashes(root)):
        raise ValueError('Missing, skipped, failed or stale validation')
    checks = validation.get('checks', [])
    if (len(checks) != len(EXPECTED_CHECKS) or {item.get('name') for item in checks} != EXPECTED_CHECKS or
        any(item.get('exit_code') != 0 for item in checks)):
        raise ValueError('Missing successful named checks')
    if (vm.get('passed') is not True or vm.get('mode') != MODE or vm.get('image_sha256') != digest(image) or
        any(vm.get(key) is not False for key in ('hardware_tested','windows_handoff_tested','secure_boot_tested','bitlocker_tested','unlock_tested'))):
        raise ValueError('Invalid VM result or scope')
    cases = vm.get('cases', [])
    if (len(cases) != len(CASES) or {item.get('name') for item in cases} != CASES or
        any(item.get('passed') is not True or item.get('returned_to_firmware') is not True for item in cases)):
        raise ValueError('Incomplete VM refusal checks')

def build(output, build_directory, validation_path, vm_path):
    image_path = build_directory/'BOOTX64.EFI'
    result_path = build_directory/'build-result.json'
    result = json.loads(result_path.read_text())
    validation = json.loads(validation_path.read_text())
    vm = json.loads(vm_path.read_text())
    generated = {name:(build_directory/name).read_bytes() for name in ('board_profile.h','rehearsal_profile.h')}
    check_inputs(image_path.read_bytes(), result, validation, vm, generated)
    files = {name:WIN/'usb-staged'/name for name in USB_FILES}
    files.update({'benchmark/'+name:WIN/'benchmark'/name for name in BENCHMARK_FILES})
    files.update({'BOOTX64.EFI':image_path, 'build-result.json':result_path,
                  'VALIDATION.json':validation_path, 'VM-VALIDATION.json':vm_path,
                  'LICENSE':ROOT/'LICENSE','THIRD_PARTY_NOTICES.md':WIN/'THIRD_PARTY_NOTICES.md'})
    names = set(result['source_sha256']) | set(validation['tested_source_sha256'])
    # Preserve all dependencies of the reproducible build, asset generator and
    # complete host suite. No raw captures, native driver, or downloaded assets.
    for folder in ('windows/src','windows/deps','windows/tests','windows/LICENSES','kernel/gpu_unlock'):
        names |= {path.relative_to(ROOT).as_posix() for path in (ROOT/folder).rglob('*')
                  if path.is_file() and path.suffix in {'.c','.h','.inc','.py','.ps1','.txt'} and
                  '__pycache__' not in path.parts}
    for name in names: files['source/'+name] = ROOT/name
    payload = {}
    for name,path in sorted(files.items()):
        if (path.is_symlink() or not path.is_file() or '/native/' in name or
            path.suffix.lower() in {'.sys','.bin','.exe','.dll'} or
            (path.suffix.lower() == '.efi' and name != 'BOOTX64.EFI')):
            raise ValueError('Missing, linked or forbidden package input: '+name)
        payload[name] = path.read_bytes()
    payload.update({'source/generated/'+name:data for name,data in generated.items()})
    payload['SHA256SUMS.json'] = (json.dumps({name:digest(data) for name,data in sorted(payload.items())},indent=2)+'\n').encode()
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,data in sorted(payload.items()):
            info = zipfile.ZipInfo(NAME+'/'+name,date_time=(2026,10,25,0,0,0))
            info.compress_type = zipfile.ZIP_DEFLATED; info.external_attr = 0o100644 << 16
            archive.writestr(info,data)
    return dict(path=str(output.resolve()),files=len(payload),bytes=output.stat().st_size,
                sha256=digest(output.read_bytes()),efi_sha256=result['sha256'])

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-directory',type=Path,default=WIN/'build'/MODE)
    parser.add_argument('--validation',type=Path,required=True)
    parser.add_argument('--vm-validation',type=Path,required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.output,args.build_directory,args.validation,args.vm_validation),indent=2))
