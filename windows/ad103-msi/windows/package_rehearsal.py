#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package only a validated, profile-bound firmware rehearsal image."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from build import verify_pe
from board_profile import header as board_header
from rehearsal_profile import rehearsal_header
from package_diagnostic import validation_source_hashes as diagnostic_hashes

ROOT=Path(__file__).resolve().parent.parent
WIN=ROOT/'windows'
NAME='gpu-unlock-windows-usb-rehearsal-0.5.2'
SOURCES={f'windows/src/{name}.c' for name in
    ('app','app_io','ownership','format','runtime','sha256','checks','port','compat','probe',
     'capture_pe','rehearsal','rehearsal_handoff')}|{
     'kernel/gpu_unlock/vbios_610.c','kernel/gpu_unlock/gfw_eval.c'}
CASES={'usb_absent_target','missing_report_directory','non_usb'}

def digest(data):return hashlib.sha256(data).hexdigest()

def validation_source_hashes(root=ROOT):
    result=diagnostic_hashes(root)
    names={'windows/package_rehearsal.py','windows/validate_rehearsal.py','windows/rehearsal_profile.py'}
    names|={p.relative_to(root).as_posix() for p in (root/'windows/rehearsal').rglob('*') if p.is_file()}
    for name in sorted(names):
        p=root/name
        if p.is_symlink() or not p.is_file():raise ValueError('Missing or linked validation input: '+name)
        result[name]=digest(p.read_bytes())
    return dict(sorted(result.items()))

def check_inputs(image,result,validation,vm,generated,root=ROOT):
    if (result.get('mode')!='firmware-rehearsal' or result.get('version')!='0.5.2-rehearsal' or
        result.get('hardware_tested') is not False or result.get('unlock_enabled') is not False or
        result.get('helper_protocol') is not None):raise ValueError('Only isolated rehearsal mode can be packaged')
    if result.get('sha256')!=digest(image) or result.get('size')!=len(image):raise ValueError('EFI/build mismatch')
    verify_pe(image)
    if set(result.get('compiled_sources',[]))!=SOURCES or len(result['compiled_sources'])!=len(SOURCES):
        raise ValueError('Unexpected compiled source set')
    for forbidden in (b'DIAGNOSTIC_ONLY:',b'Press U once',b'booter_load_ad102.bin',b'GUSSN001',b'UNLOCK_VERIFIED:',
                      b'Falcon start',b'unlock_transaction_dma',b'UNLOCK_SUCCESS'):
        if forbidden in image:raise ValueError('Unexpected mode/payload in rehearsal')
    if b'REHEARSAL_ONLY:' not in image or b'mode=firmware-rehearsal' not in image:
        raise ValueError('Rehearsal entry marker absent')
    cp=result.get('capture_profile')
    if not isinstance(cp,dict) or cp.get('unlock_authorized') is not False or cp.get('board')!=result.get('board_profile'):
        raise ValueError('Invalid capture profile')
    expected={'board_profile.h':board_header(cp['board']).encode('ascii'),
              'rehearsal_profile.h':rehearsal_header(cp).encode('ascii')}
    if generated!=expected or result.get('generated_header_sha256')!={k:digest(v) for k,v in expected.items()}:
        raise ValueError('Generated board/driver header mismatch')
    hashes=result.get('source_sha256',{})
    if not SOURCES|{'windows/build.py','windows/rehearsal_profile.py','windows/board_profile.py'} <= hashes.keys():
        raise ValueError('Incomplete build provenance')
    for name,sha in hashes.items():
        path=root/name
        if Path(name).is_absolute() or '..' in Path(name).parts or path.is_symlink() or not path.is_file() or digest(path.read_bytes())!=sha:
            raise ValueError('Source changed since build: '+name)
    if (validation.get('all_passed') is not True or validation.get('mode')!='firmware-rehearsal' or
        validation.get('hardware_tested') is not False or validation.get('windows_runtime_tested') is not False or
        validation.get('image_sha256')!=digest(image) or
        validation.get('tested_source_sha256')!=validation_source_hashes(root)):
        raise ValueError('Missing, failed or stale validation')
    checks=validation.get('checks',[])
    if len(checks)!=4 or {c.get('name') for c in checks}!={'build','host-tests','usb-tests','vm-tests'} or any(c.get('exit_code')!=0 for c in checks):
        raise ValueError('Missing successful named checks')
    if (vm.get('passed') is not True or vm.get('mode')!='firmware-rehearsal' or vm.get('image_sha256')!=digest(image) or
        any(vm.get(k) is not False for k in ('hardware_tested','windows_handoff_tested','secure_boot_tested','bitlocker_tested'))):
        raise ValueError('Invalid VM scope/result')
    cases=vm.get('cases',[])
    if len(cases)!=3 or {c.get('name') for c in cases}!=CASES or any(c.get('passed') is not True or c.get('returned_to_firmware') is not True for c in cases):
        raise ValueError('Incomplete VM refusal tests')

def build(output,build_dir,validation_path,vm_path):
    result_path=build_dir/'build-result.json';image_path=build_dir/'BOOTX64.EFI'
    result=json.loads(result_path.read_text());validation=json.loads(validation_path.read_text());vm=json.loads(vm_path.read_text())
    generated={name:(build_dir/name).read_bytes() for name in ('board_profile.h','rehearsal_profile.h')}
    check_inputs(image_path.read_bytes(),result,validation,vm,generated)
    files={name:WIN/'rehearsal'/name for name in ('Update-USB.cmd','Update-USB.ps1','RehearsalUsb.psm1','START-HERE.txt','README.md')}
    files.update({'UsbPreparation.psm1':WIN/'UsbPreparation.psm1','LICENSE':ROOT/'LICENSE',
        'THIRD_PARTY_NOTICES.md':WIN/'THIRD_PARTY_NOTICES.md','USB/EFI/BOOT/BOOTX64.EFI':image_path,
        'build-result.json':result_path,'VALIDATION.json':validation_path,'VM-VALIDATION.json':vm_path})
    names=set(result['source_sha256'])|set(validation['tested_source_sha256'])|{
        'windows/build.py','windows/README.md','windows/THIRD_PARTY_NOTICES.md'}
    # Include source dependencies for the full host suite, with no native-driver
    # installers, user reports, asset binaries or other executable payloads.
    for folder in ('windows/src','windows/deps','windows/tests','windows/LICENSES','kernel/gpu_unlock'):
        names|={p.relative_to(ROOT).as_posix() for p in (ROOT/folder).rglob('*')
            if p.is_file() and p.suffix in {'.c','.h','.inc','.py','.ps1','.txt'} and '__pycache__' not in p.parts}
    for name in names:files['source/'+name]=ROOT/name
    payload={}
    for name,path in sorted(files.items()):
        if path.is_symlink() or not path.is_file() or '/native/' in name or path.suffix.lower() in {'.sys','.bin','.exe','.dll'}:
            raise ValueError('Missing, linked or forbidden package input: '+name)
        if path.suffix.lower()=='.efi' and name!='USB/EFI/BOOT/BOOTX64.EFI':raise ValueError('Extra EFI payload')
        payload[name]=path.read_bytes()
    payload.update({'source/generated/'+name:data for name,data in generated.items()})
    payload['SHA256SUMS.json']=(json.dumps({name:digest(data) for name,data in sorted(payload.items())},indent=2)+'\n').encode()
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name,data in sorted(payload.items()):
            info=zipfile.ZipInfo(NAME+'/'+name,date_time=(2026,10,25,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=0o100644<<16;z.writestr(info,data)
    return {'path':str(output.resolve()),'files':len(payload),'bytes':output.stat().st_size,'sha256':digest(output.read_bytes()),'efi_sha256':result['sha256']}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--build-directory',type=Path,default=WIN/'build/firmware-rehearsal')
    p.add_argument('--validation',type=Path,required=True);p.add_argument('--vm-validation',type=Path,required=True)
    a=p.parse_args();print(json.dumps(build(a.output,a.build_directory,a.validation,a.vm_validation),indent=2))
