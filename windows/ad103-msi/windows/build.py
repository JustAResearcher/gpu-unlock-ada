#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an x64 UEFI diagnostic, or an explicitly requested profile-bound experiment."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from board_profile import parse_report, header

ROOT = Path(__file__).resolve().parent.parent
WIN = ROOT/'windows'

def verify_pe(data: bytes) -> dict:
    def u16(o): return struct.unpack_from('<H', data, o)[0]
    def u32(o): return struct.unpack_from('<I', data, o)[0]
    if len(data)<512 or data[:2]!=b'MZ': raise ValueError('not a PE image')
    pe=u32(60); opt=pe+24
    if data[pe:pe+4]!=b'PE\0\0' or u16(pe+4)!=0x8664 or u16(opt)!=0x20b:
        raise ValueError('not AMD64 PE32+')
    if u16(opt+68)!=10 or u16(opt+70)&0x140!=0x140:
        raise ValueError('UEFI subsystem/ASLR/NX missing')
    if not u32(opt+112+5*8+4): raise ValueError('relocations missing')
    for i in (1,12,13):
        if data[opt+112+i*8:opt+120+i*8]!=b'\0'*8:
            raise ValueError('unexpected import directory')
    entry=u32(opt+16); sections=opt+u16(pe+20)
    if not any(u32(t+12)<=entry<u32(t+12)+u32(t+8) and u32(t+36)&0x20000000
               for t in [sections+40*i for i in range(u16(pe+6))]):
        raise ValueError('entry point is not executable')
    return {'machine':'AMD64','subsystem':'EFI_APPLICATION','imports':False,'relocations':True}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig', default='zig', help='Zig 0.14.1 executable')
    p.add_argument('--profile',type=Path,help='completed diagnostic REPORT.TXT')
    p.add_argument('--experimental',action='store_true')
    p.add_argument('--rehearsal',action='store_true',help='build active firmware release/restoration rehearsal; no unlock payload')
    p.add_argument('--topology',type=Path,help='0.4.2 TOPOLOGY.TXT required with --rehearsal')
    p.add_argument('--restoration-report',type=Path,help='completed 0.5.1 stage-4 framebuffer refusal required with --rehearsal')
    p.add_argument('--session-helper',action='store_true',
                   help='build gu-session-v1 one-shot ESP helper; requires experimental exact profile')
    p.add_argument('--output',type=Path)
    a=p.parse_args()
    if a.rehearsal and (a.experimental or a.session_helper or not a.profile or not a.topology or not a.restoration_report):
        p.error('--rehearsal requires --profile, --topology and --restoration-report, and cannot enable experimental/session modes')
    if (a.topology or a.restoration_report) and not a.rehearsal:
        p.error('--topology and --restoration-report are only accepted with --rehearsal')
    if not a.rehearsal and a.experimental != bool(a.profile):
        p.error('experimental mode requires BOTH --experimental and --profile REPORT.TXT')
    if a.session_helper and not a.experimental:
        p.error('--session-helper requires --experimental and --profile REPORT.TXT')
    version=subprocess.check_output([a.zig,'version'],text=True).strip()
    if version!='0.14.1': p.error('reproducible builds require Zig 0.14.1')
    mode='firmware-rehearsal' if a.rehearsal else 'experimental-session' if a.session_helper else 'experimental-unlock' if a.experimental else 'diagnostic'
    out=(a.output or WIN/'build'/mode).resolve();out.mkdir(parents=True,exist_ok=True)
    capture_profile=None
    if a.rehearsal:
        from rehearsal_profile import parse_capture, rehearsal_header, bind_restoration_report
        capture_profile=bind_restoration_report(parse_capture(a.profile.read_bytes(),a.topology.read_bytes()),a.restoration_report.read_bytes())
        profile=capture_profile['board']
        (out/'rehearsal_profile.h').write_text(rehearsal_header(capture_profile),encoding='ascii',newline='\n')
    else:
        profile=parse_report(a.profile.read_bytes()) if a.profile else None
    if profile:(out/'board_profile.h').write_text(header(profile),encoding='ascii',newline='\n')
    port=['app','app_io','ownership','format','runtime','sha256','checks','port','compat','probe']
    # Do not link the unlock transaction, firmware loader or session machinery
    # into the diagnostic, even as unreachable functions. Write adapters remain
    # compile-time denials as a separate layer of protection.
    core=['vbios_610','gfw_eval']
    if a.experimental:
        core=['unlock_610','prep_610','layout_610']+core
    elif a.rehearsal:
        port += ['capture_pe', 'rehearsal', 'rehearsal_handoff']
    else:
        port += ['capture', 'capture_pe']
    if a.session_helper:
        port.append('session')
    sources=[WIN/'src'/f'{x}.c' for x in port]+[ROOT/'kernel/gpu_unlock'/f'{x}.c' for x in core]
    image=out/'BOOTX64.EFI'
    cmd=[a.zig,'cc','-target','x86_64-uefi','-std=c11','-ffreestanding','-fno-builtin',
         '-fno-stack-protector','-mno-red-zone','-fshort-wchar','-Os','-g0','-Wall','-Wextra',
         '-Werror','-Wno-unused-parameter','-D__KERNEL__','-DGU_UEFI_PORT',
         '-ffile-prefix-map='+str(ROOT)+'=.',
         f'-DGU_ENABLE_UNLOCK={int(a.experimental)}',f'-DGU_REHEARSAL={int(a.rehearsal)}',f'-DGU_SESSION_HELPER={int(a.session_helper)}','-I'+str(out),
         '-I'+str(WIN/'src/compat'),'-I'+str(ROOT/'kernel/gpu_unlock'),
         '-I'+str(WIN/'deps/edk2'),'-I'+str(WIN/'deps/edk2/X64'),'-nostdlib']
    subprocess.run(cmd+[x.relative_to(ROOT).as_posix() for x in sources]+['-o',str(image)],
                   cwd=ROOT,check=True)
    data=image.read_bytes()
    result={'version':'0.5.2-rehearsal' if a.rehearsal else '0.4.2-diagnostic' if not a.experimental else '0.1-test','mode':mode,'sha256':hashlib.sha256(data).hexdigest(),
            'size':len(data),'compiler':'zig '+version,'hardware_tested':False,
            'board_profile':profile,'capture_profile':capture_profile,'unlock_enabled':bool(a.experimental),
            'helper_protocol':'gu-session-v1' if a.session_helper else None,
            'pe':verify_pe(data),'compiled_sources':[str(x.relative_to(ROOT)) for x in sources],
            'source_sha256':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest()
                for x in sorted({Path(__file__).resolve()}|set(sources)|set((WIN/'src').rglob('*.h'))|
                                set((WIN/'src').rglob('*.inc'))|
                                set((ROOT/'kernel/gpu_unlock').glob('*.h'))|
                                set((WIN/'deps/edk2').rglob('*.h')))}}
    if a.rehearsal:
        result['generated_header_sha256']={name:hashlib.sha256((out/name).read_bytes()).hexdigest()
            for name in ('board_profile.h','rehearsal_profile.h')}
        result['source_sha256']['windows/rehearsal_profile.py']=hashlib.sha256((WIN/'rehearsal_profile.py').read_bytes()).hexdigest()
        result['source_sha256']['windows/board_profile.py']=hashlib.sha256((WIN/'board_profile.py').read_bytes()).hexdigest()
    (out/'build-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('source_sha256','board_profile','capture_profile')},indent=2))

if __name__=='__main__':main()
