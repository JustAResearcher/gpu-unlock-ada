#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare pinned UEFI assets on Windows/Linux; never install or execute a driver."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT))
from firmware.fetch_artifacts import fetch,inputs,MANIFEST
from firmware.extract_boot_assets import ADA_PINNED,extract_assets
from firmware.generate_v67 import (build_ad102_payload,AD102_PAYLOAD_NAME,
    AD102_PAYLOAD_SHA256,GSP_SHA256,FIRMWARE_PREFIX,PACKAGE_VERSION)

def selected_members(archive,expected):
    """Read only named regular files; never extract archive paths to disk."""
    result={}
    for member in archive:
        if member.name not in expected:continue
        if member.name in result or not member.isfile() or member.size>128*1024*1024:
            raise ValueError('invalid/duplicate archive member: '+member.name)
        stream=archive.extractfile(member)
        if stream is None:raise ValueError('unreadable archive member')
        data=stream.read()
        if len(data)!=member.size:raise ValueError('truncated archive member')
        digest=expected[member.name]
        if digest and hashlib.sha256(data).hexdigest()!=digest:
            raise ValueError('member SHA-256 mismatch: '+member.name)
        result[member.name]=data
    if set(result)!=set(expected):raise ValueError('required archive members missing')
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache',type=Path,default=ROOT/'windows/.cache/inputs')
    p.add_argument('--output',type=Path,required=True,help='new empty ASSETS directory')
    p.add_argument('--offline',action='store_true')
    a=p.parse_args()
    if a.output.exists() and any(a.output.iterdir()):p.error('output directory must be empty')
    try:import zstandard
    except ImportError:p.error('install Python dependency: py -m pip install zstandard==0.23.0')
    manifest=json.loads(MANIFEST.read_text());entries=inputs(manifest)
    files=[]
    for entry in entries:
        print('Verify/download: '+entry['filename'],flush=True)
        files.append(fetch(entry,a.cache,a.offline))
    source,package=files
    prefix='NVIDIA-kernel-module-source-610.43.03/src/nvidia/generated/'
    with tarfile.open(source,'r:xz') as archive:
        generated=selected_members(archive,{prefix+n:h for n,h in ADA_PINNED.items()})
    notice=entries[1]['notice']
    expected={'.PKGINFO':None,FIRMWARE_PREFIX+'gsp_ga10x.bin':GSP_SHA256,
              notice['path']:notice['sha256']}
    with package.open('rb') as raw,zstandard.ZstdDecompressor().stream_reader(raw) as reader:
        with tarfile.open(fileobj=reader,mode='r|') as archive:
            members=selected_members(archive,expected)
    if f'pkgver = {PACKAGE_VERSION}' not in members['.PKGINFO'].decode('utf-8').splitlines():
        raise ValueError('package metadata version mismatch')
    payload=build_ad102_payload()
    if hashlib.sha256(payload).hexdigest()!=AD102_PAYLOAD_SHA256:
        raise ValueError('payload SHA-256 mismatch')
    # Nothing is published until every input and derived asset is checked.
    with tempfile.TemporaryDirectory() as td:
        temp=Path(td);src=temp/'src/nvidia/generated';src.mkdir(parents=True)
        for name,data in generated.items():(src/Path(name).name).write_bytes(data)
        out=temp/'assets';extract_assets(temp,out,'ad102')
        (out/'gsp_ga10x.bin').write_bytes(members[FIRMWARE_PREFIX+'gsp_ga10x.bin'])
        (out/AD102_PAYLOAD_NAME).write_bytes(payload)
        (out/'NVIDIA-LICENSE').write_bytes(members[notice['path']])
        shutil.copy2(ROOT/'firmware/NOTICE.md',out/'NOTICE.md')
        a.output.mkdir(parents=True,exist_ok=True)
        for f in out.iterdir():shutil.copy2(f,a.output/f.name)
    print('Verified assets ready: '+str(a.output.resolve()))

if __name__=='__main__':main()
