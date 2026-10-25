#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate a capture-bound rehearsal; emulation covers refusal only."""
from __future__ import annotations
import argparse
from datetime import datetime,timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from package_rehearsal import ROOT,validation_source_hashes

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('zig','pwsh'):p.add_argument('--'+name,required=True)
    for name in ('profile','topology','restoration-report','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--qemu-tool-root',type=Path);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    record={'schema_version':1,'created_utc':datetime.now(timezone.utc).isoformat(),'mode':'firmware-rehearsal',
        'all_passed':False,'hardware_tested':False,'windows_runtime_tested':False,'image_sha256':None,
        'tested_source_sha256':validation_source_hashes(),'checks':[],
        'limitations':['No physical GPU, motherboard, USB, driver release/restoration or Windows handoff tested.',
            'Host tests use synthetic EFI services, images and storage; QEMU has no target GPU.',
            'VM verifies EFI loading and refusal/return only; no Secure Boot or BitLocker enforcement tested.',
            'No unlock payload or Windows kernel driver is included.']}
    env=os.environ.copy();env['PWSH']=a.pwsh
    def run(name,cmd,timeout=600):
        r=subprocess.run(cmd,cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout)
        log=r.stdout+r.stderr;(out/(name+'.log')).write_text(log)
        # Input report paths remain local; only hashes/pins are distributed.
        display=[('<capture-input>' if str(v) in {str(a.profile.resolve()),str(a.topology.resolve()),str(a.restoration_report.resolve())} else str(v)) for v in cmd]
        record['checks'].append({'name':name,'command':display,'exit_code':r.returncode,'output_sha256':hashlib.sha256(log.encode()).hexdigest()})
        print(name+': '+('PASS' if r.returncode==0 else 'FAIL'),flush=True)
        if r.returncode:print(log[-14000:],file=sys.stderr);raise RuntimeError(name+' failed')
    try:
        run('build',[sys.executable,'windows/build.py','--zig',a.zig,'--rehearsal','--profile',str(a.profile.resolve()),'--topology',str(a.topology.resolve()),'--restoration-report',str(a.restoration_report.resolve())])
        build=ROOT/'windows/build/firmware-rehearsal';record['image_sha256']=hashlib.sha256((build/'BOOTX64.EFI').read_bytes()).hexdigest()
        run('host-tests',[sys.executable,'-m','unittest','discover','-s','windows/tests','-p','test_*.py','-v'])
        run('usb-tests',[a.pwsh,'-NoProfile','-File','windows/tests/Test-RehearsalUsb.ps1'])
        cmd=[sys.executable,'windows/tests/vm_diagnostic_smoke.py','--rehearsal','--image',str(build/'BOOTX64.EFI'),'--build-result',str(build/'build-result.json'),'--output',str(out/'vm')]
        if a.qemu_tool_root:cmd+=['--tool-root',str(a.qemu_tool_root.resolve())]
        run('vm-tests',cmd,360)
        if record['tested_source_sha256']!=validation_source_hashes():raise RuntimeError('Validation sources changed during checks')
        record['all_passed']=True
    finally:(out/'VALIDATION.json').write_text(json.dumps(record,indent=2)+'\n')
    print(out/'VALIDATION.json')

if __name__=='__main__':main()
