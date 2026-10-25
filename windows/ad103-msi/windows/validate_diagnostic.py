#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the diagnostic, run host checks and boot its exact bytes in QEMU."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from package_diagnostic import validation_source_hashes

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True)
    parser.add_argument('--pwsh', required=True)
    parser.add_argument('--qemu-tool-root', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='new validation directory')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    record = {'schema_version': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
              'all_passed': False, 'hardware_tested': False, 'windows_runtime_tested': False,
              'image_sha256': None, 'checks': [],
              'tested_source_sha256': validation_source_hashes(ROOT), 'limitations': [
                  'No physical NVIDIA GPU, motherboard, or USB was tested.',
                  'Windows PowerShell 5.1 and real Windows disk discovery were not executed here.',
                  'QEMU cases test firmware refusal/return, not Windows handoff.',
                  'Secure Boot enforcement and BitLocker suspension/resumption are not tested.',
                  'The diagnostic does not attempt unlocking.']}
    env = os.environ.copy()
    env['PWSH'] = args.pwsh

    def run(name, command, timeout=300):
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
        text = result.stdout + result.stderr
        (out / (name + '.log')).write_text(text)
        record['checks'].append({'name': name, 'command': command, 'exit_code': result.returncode,
                                 'output_sha256': hashlib.sha256(text.encode()).hexdigest()})
        print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'), flush=True)
        if result.returncode:
            print(text[-10000:], file=sys.stderr)
            raise RuntimeError(name + ' failed')

    try:
        run('build', [sys.executable, 'windows/build.py', '--zig', args.zig])
        build = ROOT / 'windows/build/diagnostic'
        record['image_sha256'] = hashlib.sha256((build / 'BOOTX64.EFI').read_bytes()).hexdigest()
        run('uefi-host-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'windows/tests', '-p', 'test_*.py', '-v'])
        run('usb-preparation-tests', [args.pwsh, '-NoProfile', '-File', 'windows/tests/Test-UsbPreparation.ps1'])
        command = [sys.executable, 'windows/tests/vm_diagnostic_smoke.py', '--image', str(build / 'BOOTX64.EFI'),
                   '--build-result', str(build / 'build-result.json'), '--output', str(out / 'vm')]
        if args.qemu_tool_root:
            command += ['--tool-root', str(args.qemu_tool_root.resolve())]
        run('uefi-vm-tests', command, timeout=360)
        if validation_source_hashes(ROOT) != record['tested_source_sha256']:
            raise RuntimeError('Host test or USB preparation sources changed during validation; rerun the checks.')
        record['all_passed'] = True
    finally:
        (out / 'VALIDATION.json').write_text(json.dumps(record, indent=2) + '\n')
    print(str(out / 'VALIDATION.json'))


if __name__ == '__main__':
    main()
