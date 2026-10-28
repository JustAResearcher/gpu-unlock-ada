"""Create a new USB-layout directory and ZIP from a verified diagnostic build."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
import targets
from verify import validate_pe, validate_source


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('destination',type=Path)
    args=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    destination=args.destination.resolve()
    archive=destination.parent/(destination.name+'.zip')
    if destination.exists() or archive.exists():
        p.error('Destination or ZIP already exists; use a new output name')
    target=targets.selected()
    build=root/target['build']
    manifest=json.loads((build/'manifest.json').read_text(encoding='utf-8-sig'))
    if manifest.get('hardware_tested') is not False or manifest.get('gpu_write_capability') is not False:
        p.error('Expected an unvalidated, diagnostic-only build')
    binary=build/target['binary']
    if digest(binary)!=manifest['sha256']:
        p.error('Binary hash mismatch')
    for name,expected in manifest['source_sha256'].items():
        if Path(name).name!=name or digest(root/'src'/name)!=expected:
            p.error('Source hash mismatch: '+name)
    headers=json.loads((root/'vendor/edk2/headers.json').read_text())
    for name,expected in headers.items():
        path=(root/'vendor/edk2/Include'/name).resolve()
        if not path.is_relative_to((root/'vendor/edk2/Include').resolve()) or digest(path)!=expected:
            p.error('Header hash mismatch: '+name)
    validate_pe(binary.read_bytes())
    validate_source((root/'src/probe.c').read_text())
    boot=destination/'EFI/BOOT'
    boot.mkdir(parents=True)
    shutil.copyfile(binary,boot/'BOOTX64.EFI')
    shutil.copyfile(build/'manifest.json',destination/'MANIFEST.json')
    shutil.copyfile(root/'README.md',destination/'README-FIRST.md')
    sources=destination/'sources'
    sources.mkdir()
    for name in ('src','tests','tools','vendor'):
        shutil.copytree(root/name,sources/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    for name in ('Build.ps1','README.md','AD104-STATUS.md','LICENSE.txt','THIRD_PARTY.md'):
        shutil.copyfile(root/name,sources/name)
    files={str(path.relative_to(destination)).replace('\\','/'):digest(path)
           for path in sorted(destination.rglob('*')) if path.is_file()}
    (destination/'SHA256SUMS.json').write_text(json.dumps(files,indent=2),encoding='utf-8')
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED) as z:
        for path in sorted(destination.rglob('*')):
            if path.is_file():
                z.write(path,path.relative_to(destination).as_posix())
    print('USB layout:',destination)
    print('Archive:',archive)
    print('EFI SHA256:',manifest['sha256'])


if __name__=='__main__':
    main()
