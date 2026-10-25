#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the explicitly enabled, evidence-bound staged Windows unlock test.

This separate entry point cannot change the default diagnostic/rehearsal modes.
Firmware assets are verified locally; it never downloads or executes them.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

from board_profile import header as board_header
from build import verify_pe
from rehearsal_profile import parse_capture, bind_restoration_report
from staged_profile import bind_rehearsal_success, staged_header

ROOT = Path(__file__).resolve().parent.parent
WIN = ROOT/'windows'
MODE = 'staged-unlock-test'
VERSION = '0.6.4-unlock-test'
DEFINES = {'GU_ENABLE_UNLOCK':1,'GU_STAGED_UNLOCK':1,'GU_REHEARSAL':0,'GU_SESSION_HELPER':0}
PORT = ('app','app_io','ownership','format','runtime','sha256','checks','port','compat','probe',
        'capture_pe','rehearsal','rehearsal_handoff','staged_state')
CORE = ('unlock_610','prep_610','layout_610','vbios_610','gfw_eval')
SOURCES = {f'windows/src/{name}.c' for name in PORT}|{f'kernel/gpu_unlock/{name}.c' for name in CORE}
REQUIRED_ASSETS = {
    'booter_load_ad102.bin': {'bytes':57672,'sha256':'6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49'},
    'v67-ad102-610.43.03-four-plm.bin': {'bytes':64000,'sha256':'44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb'},
    'gsp_ga10x.bin': {'bytes':84277400,'sha256':'572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a'},
    'gsp_boot_ad102.bin': {'bytes':36864,'sha256':'45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0'},
    'gsp_boot_desc_ad102.bin': {'bytes':84,'sha256':'83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42'},
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_assets(directory: Path) -> dict:
    """Require all five exact regular files; no synthetic/missing-asset fallback."""
    directory = Path(directory)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('Asset directory is missing or linked')
    for name,expected in REQUIRED_ASSETS.items():
        path = directory/name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing or linked staged asset: '+name)
        with path.open('rb') as file:
            before = os.fstat(file.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_size != expected['bytes']:
                raise ValueError('Staged asset size/type mismatch: '+name)
            hashed = hashlib.sha256(); size = 0
            while block := file.read(1024*1024):
                size += len(block)
                if size > expected['bytes']: raise ValueError('Staged asset grew during verification: '+name)
                hashed.update(block)
            after = os.fstat(file.fileno())
        current = path.stat()
        identity = lambda value:(value.st_dev,value.st_ino,value.st_size,value.st_mtime_ns)
        if (size != expected['bytes'] or hashed.hexdigest() != expected['sha256']
                or identity(before) != identity(after) or identity(before) != identity(current)
                or path.is_symlink()):
            raise ValueError('Staged asset changed or SHA-256 mismatch: '+name)
    return copy.deepcopy(REQUIRED_ASSETS)


def build_source_hashes(root: Path = ROOT) -> dict[str,str]:
    names = SOURCES | {'windows/build.py','windows/build_staged.py','windows/board_profile.py',
                       'windows/rehearsal_profile.py','windows/staged_profile.py'}
    for directory,pattern in (('windows/src','*.h'),('windows/src','*.inc'),
                              ('kernel/gpu_unlock','*.h'),('windows/deps/edk2','*.h')):
        names |= {path.relative_to(root).as_posix() for path in (root/directory).rglob(pattern)}
    result = {}
    for name in sorted(names):
        path = root/name
        if path.is_symlink() or not path.is_file(): raise ValueError('Missing or linked build source: '+name)
        result[name] = digest(path.read_bytes())
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',default='zig',help='exact Zig 0.14.1 executable')
    for name in ('profile','topology','restoration-report','rehearsal-report'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-boot-confirmed',action='store_true',
                        help='explicit user confirmation that the prior 0.5.2 rehearsal booted Windows')
    parser.add_argument('--assets',type=Path,default=os.environ.get('GPU_UNLOCK_ASSETS'),
                        help='five exact verified firmware files; defaults to GPU_UNLOCK_ASSETS')
    parser.add_argument('--output',type=Path,default=WIN/'build'/MODE)
    args = parser.parse_args()
    if not args.windows_boot_confirmed:
        parser.error('--windows-boot-confirmed must reflect a separate user observation, not the pending-handoff log')
    if args.assets is None: parser.error('--assets or GPU_UNLOCK_ASSETS is required; no asset-free staged build')
    assets = verify_assets(args.assets)
    profile = parse_capture(args.profile.read_bytes(),args.topology.read_bytes())
    profile = bind_restoration_report(profile,args.restoration_report.read_bytes())
    profile = bind_rehearsal_success(profile,args.rehearsal_report.read_bytes(),args.windows_boot_confirmed)
    version = subprocess.check_output([args.zig,'version'],text=True).strip()
    if version != '0.14.1': parser.error('reproducible builds require Zig 0.14.1')
    source_hashes = build_source_hashes()
    output = args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    generated = {'board_profile.h':board_header(profile['board']), 'rehearsal_profile.h':staged_header(profile)}
    for name,text in generated.items(): (output/name).write_text(text,encoding='ascii',newline='\n')
    image = output/'BOOTX64.EFI'
    sources = [f'windows/src/{name}.c' for name in PORT]+[f'kernel/gpu_unlock/{name}.c' for name in CORE]
    command = [args.zig,'cc','-target','x86_64-uefi','-std=c11','-ffreestanding','-fno-builtin',
               '-fno-stack-protector','-mno-red-zone','-fshort-wchar','-Os','-g0','-Wall','-Wextra',
               '-Werror','-Wno-unused-parameter','-D__KERNEL__','-DGU_UEFI_PORT',
               '-ffile-prefix-map='+str(ROOT)+'=.']
    command += ['-D'+name+'='+str(value) for name,value in DEFINES.items()]
    command += ['-I'+str(output),'-I'+str(WIN/'src/compat'),'-I'+str(ROOT/'kernel/gpu_unlock'),
                '-I'+str(WIN/'deps/edk2'),'-I'+str(WIN/'deps/edk2/X64'),'-nostdlib']
    subprocess.run(command+sources+['-o',str(image)],cwd=ROOT,check=True)
    if source_hashes != build_source_hashes(): raise RuntimeError('Sources changed while building staged image')
    if assets != verify_assets(args.assets): raise RuntimeError('Assets changed while building staged image')
    data = image.read_bytes()
    result = dict(version=VERSION,mode=MODE,sha256=digest(data),size=len(data),compiler='zig '+version,
                  hardware_tested=False,unlock_enabled=True,staged_unlock_enabled=True,helper_protocol=None,
                  board_profile=profile['board'],capture_profile=profile,build_defines=DEFINES,
                  assets_verified=True,required_assets=assets,pe=verify_pe(data),compiled_sources=sources,
                  source_sha256=source_hashes,
                  generated_header_sha256={name:digest(text.encode('ascii')) for name,text in generated.items()})
    (output/'build-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({key:value for key,value in result.items()
                      if key not in ('source_sha256','board_profile','capture_profile','required_assets')},indent=2))


if __name__ == '__main__': main()
