#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the explicit staged unlock build without executing on GPU hardware.

Release evidence requires real, exact pinned firmware assets and execution of
the shared-core host transaction fixture. A skipped asset test cannot pass.
The VM checks refusal only and receives neither assets nor a physical GPU.
"""
from __future__ import annotations

import argparse
from datetime import datetime,timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from build_staged import MODE,ROOT,WIN,verify_assets
from package_rehearsal import validation_source_hashes as rehearsal_source_hashes

REAL_TRANSACTION_MARKER = 'real shared core + real Windows EFI adapters: PASS'
REAL_ASSET_IO_MARKER = ('real app_io pinned-asset loader executed: 5 assets; '
                        '84277400-byte GSP; SHA256 and EOF verified')
EXPECTED_CHECKS = {'build','host-tests','usb-tests','vm-tests'}
BENCHMARK_FILES = {'run_benchmark.py','kernels.ptx','README.md'}


def verify_critical_host_checks(log: str) -> dict[str,bool]:
    """Both real-asset paths must execute once; a skipped unittest is insufficient."""
    if log.count(REAL_TRANSACTION_MARKER) != 1:
        raise RuntimeError('Real shared-core transaction did not execute exactly once; skipped tests are insufficient')
    if log.count(REAL_ASSET_IO_MARKER) != 1:
        raise RuntimeError('Real pinned-asset loader did not execute exactly once; skipped tests are insufficient')
    return {'real_transaction_verified':True,'real_asset_io_verified':True}


def validation_source_hashes(root: Path = ROOT) -> dict[str,str]:
    result = rehearsal_source_hashes(root)
    names = {'windows/build_staged.py','windows/validate_staged.py',
             'windows/staged_profile.py','windows/package_staged.py','windows/README.md'}
    folder = root/'windows/usb-staged'
    if not folder.is_dir() or folder.is_symlink(): raise ValueError('Missing or linked staged USB source directory')
    names |= {path.relative_to(root).as_posix() for path in folder.rglob('*') if path.is_file()}
    benchmark = root/'windows/benchmark'
    if not benchmark.is_dir() or benchmark.is_symlink():
        raise ValueError('Missing or linked benchmark source directory')
    actual = {path.relative_to(benchmark).as_posix() for path in benchmark.rglob('*')
              if (path.is_file() or path.is_symlink()) and '__pycache__' not in path.parts}
    if actual != BENCHMARK_FILES:
        raise ValueError('Unexpected benchmark source inventory')
    names |= {'windows/benchmark/'+name for name in BENCHMARK_FILES}
    for name in sorted(names):
        path = root/name
        if path.is_symlink() or not path.is_file(): raise ValueError('Missing or linked staged validation source: '+name)
        result[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return dict(sorted(result.items()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('zig','pwsh'): parser.add_argument('--'+name,required=True)
    for name in ('profile','topology','restoration-report','rehearsal-report','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-boot-confirmed',action='store_true')
    parser.add_argument('--assets',type=Path,default=os.environ.get('GPU_UNLOCK_ASSETS'))
    parser.add_argument('--build-directory',type=Path,default=WIN/'build'/MODE)
    parser.add_argument('--qemu-tool-root',type=Path)
    args = parser.parse_args()
    if not args.windows_boot_confirmed:
        parser.error('--windows-boot-confirmed requires the user-observed prior rehearsal boot')
    if args.assets is None: parser.error('--assets or GPU_UNLOCK_ASSETS is required for release validation')
    assets_path = args.assets.resolve()
    assets = verify_assets(args.assets)
    output = args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    source_hashes = validation_source_hashes()
    record = dict(schema_version=1,created_utc=datetime.now(timezone.utc).isoformat(),mode=MODE,
                  all_passed=False,hardware_tested=False,windows_runtime_tested=False,
                  image_sha256=None,tested_source_sha256=source_hashes,checks=[],
                  real_transaction_verified=False,real_asset_io_verified=False,
                  assets_verified=True,required_assets=assets,
                  limitations=[
                      'No actual GPU unlock, physical driver restoration after unlock, or Windows handoff tested.',
                      'Host transaction uses exact firmware files with simulated GPU registers and real shared core/adapters.',
                      'Host EFI ownership and storage services are synthetic; they do not establish silicon support.',
                      'VM checks EFI loading, storage/target refusal and firmware return only; no GPU or assets attached.',
                      'Secure Boot and BitLocker enforcement are not tested; no Windows kernel driver is installed.'])
    env = os.environ.copy(); env['PWSH'] = args.pwsh; env['GPU_UNLOCK_ASSETS'] = str(assets_path)
    captures = {str(getattr(args,name).resolve()) for name in
                ('profile','topology','restoration_report','rehearsal_report')}

    def run(name,command,timeout=600):
        try:
            result = subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout)
            log = result.stdout+result.stderr; code = result.returncode
        except subprocess.TimeoutExpired as error:
            def text(value): return value.decode(errors='replace') if isinstance(value,bytes) else value or ''
            log = text(error.stdout)+text(error.stderr)+'\nValidation command timed out.\n'; code = None
        (output/(name+'.log')).write_text(log)
        display = [('<capture-input>' if str(v) in captures else '<asset-input>' if str(v) == str(assets_path) else str(v))
                   for v in command]
        record['checks'].append(dict(name=name,command=display,exit_code=code,
                                    output_sha256=hashlib.sha256(log.encode()).hexdigest()))
        print(name+': '+('PASS' if code == 0 else 'FAIL'),flush=True)
        if code != 0:
            print(log[-14000:],file=sys.stderr)
            raise RuntimeError(name+' failed')
        return log

    try:
        command = [sys.executable,'windows/build_staged.py','--zig',args.zig,'--windows-boot-confirmed',
                   '--assets',str(assets_path),'--output',str(args.build_directory.resolve())]
        for name in ('profile','topology','restoration_report','rehearsal_report'):
            command += ['--'+name.replace('_','-'),str(getattr(args,name).resolve())]
        run('build',command)
        build = args.build_directory.resolve()
        record['image_sha256'] = hashlib.sha256((build/'BOOTX64.EFI').read_bytes()).hexdigest()
        host_log = run('host-tests',[sys.executable,'-m','unittest','discover','-s','windows/tests','-p','test_*.py','-v'])
        record.update(verify_critical_host_checks(host_log))
        run('usb-tests',[args.pwsh,'-NoProfile','-File','windows/tests/Test-StagedUsb.ps1'])
        command = [sys.executable,'windows/tests/vm_diagnostic_smoke.py','--staged','--image',str(build/'BOOTX64.EFI'),
                   '--build-result',str(build/'build-result.json'),'--output',str(output/'vm')]
        if args.qemu_tool_root: command += ['--tool-root',str(args.qemu_tool_root.resolve())]
        run('vm-tests',command,360)
        if assets != verify_assets(assets_path): raise RuntimeError('Assets changed during validation')
        if source_hashes != validation_source_hashes(): raise RuntimeError('Validation sources changed during checks')
        record['all_passed'] = True
    finally:
        (output/'VALIDATION.json').write_text(json.dumps(record,indent=2)+'\n')
    print(output/'VALIDATION.json')


if __name__ == '__main__': main()
