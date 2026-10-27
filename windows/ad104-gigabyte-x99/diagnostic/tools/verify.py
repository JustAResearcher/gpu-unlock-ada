"""Reject an unsuitable PE or an unexpected hardware-access call in probe sources."""
from pathlib import Path
import json
import re
import struct

def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_pe(p):
    require(len(p)>=64 and p[:2]==b'MZ', 'Missing DOS header')
    pe=struct.unpack_from('<I',p,0x3c)[0]
    require(pe+264<=len(p) and p[pe:pe+4]==b'PE\0\0', 'Invalid PE header')
    machine,sections=struct.unpack_from('<HH',p,pe+4)
    require(machine==0x8664 and 1<=sections<=16, 'Invalid machine or sections')
    optional_size=struct.unpack_from('<H',p,pe+20)[0]
    opt=pe+24
    require(optional_size>=240 and opt+optional_size+40*sections<=len(p), 'Truncated PE')
    require(struct.unpack_from('<H',p,opt)[0]==0x20b, 'Expected PE32+')
    require(struct.unpack_from('<H',p,opt+68)[0]==10, 'Expected EFI application')
    entry=struct.unpack_from('<I',p,opt+16)[0]
    require(entry!=0, 'Missing entry')
    for slot,label in ((1,'imports'),(9,'TLS'),(14,'CLR')):
        require(struct.unpack_from('<II',p,opt+112+slot*8)==(0,0), label+' forbidden')
    require(struct.unpack_from('<II',p,opt+112+5*8)[0]!=0, 'Relocations required')
    executable=False
    for i in range(sections):
        off=opt+optional_size+i*40
        size,rva,raw_size,raw_off=struct.unpack_from('<IIII',p,off+8)
        flags=struct.unpack_from('<I',p,off+36)[0]
        require(raw_off+raw_size<=len(p), 'Section outside file')
        if rva<=entry<rva+size:
            executable=bool(flags&0x20000000)
    require(executable,'Non-executable entry')


def validate_source(src):
    # Regression check, supplementary to inspection and host mocks.
    for pattern in (r'->(?:Mem|Io)\s*\.', r'->Pci\s*\.\s*Write',
                    r'->(?:Map|Unmap|AllocateBuffer|FreeBuffer|Attributes|SetBarAttributes|PollMem|PollIo|CopyMem)\s*\(',
                    r'->(?:SetVariable|ResetSystem|DisconnectController|ConnectController|StartImage|LoadImage|ExitBootServices|SetMode|Blt)\s*\('):
        require(not re.search(pattern,src),'Forbidden call: '+pattern)


def main():
    root=Path(__file__).resolve().parents[1]
    validate_pe((root/'build/AD104-PROBE.EFI').read_bytes())
    validate_source((root/'src/probe.c').read_text())
    validate_source((root/'src/platform.c').read_text())
    sample=(root/'src/sample.c').read_text()
    validate_source(sample.replace('->Mem.Read','->ApprovedRead'))
    require(sample.count('pci->Mem.Read(')==6,'Expected register and bounded PROM read call sites')
    offsets=[int(x,16) for x in re.findall(r'\{"[A-Z0-9_]+",(0x[0-9a-f]+)\}',sample)]
    require(offsets==[0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100],'Register allowlist changed')
    capture=json.loads((root/'build/host-capture.json').read_text())
    require(capture['schema']==4 and not capture['gpu_writes'] and not capture['unlock_attempted'],'Mock flags')
    require(capture['unique_expected_identity_observed'] and len(capture['display_devices'])==2,'Mock identity')
    require(bool(capture['display_devices'][0]['cached_efi_rom']['sha256']),'Mock ROM')
    require([g['display_parent_index'] for g in capture['gops']]==[0,1],'Mock GOP ancestry')
    require(capture['register_snapshot']['complete'] and capture['register_snapshot']['read_attempts']==23,'Mock sampler')
    require(capture['physical_prom']['complete'] and len(capture['physical_prom']['hex'])==2*1024*1024,'Mock physical ROM')
    print('PASS: x64 EFI PE, no imports/TLS/CLR, relocations and executable entry; source access checks; mock JSON schema')


if __name__=='__main__':
    main()
