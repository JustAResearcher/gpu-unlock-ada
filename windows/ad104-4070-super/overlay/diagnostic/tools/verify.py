"""Reject an unsuitable PE or an unexpected hardware-access call in probe sources."""
from pathlib import Path
import json
import re
import struct
import hashlib

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


def validate_source(src, allow_mmio_reads=False):
    # Narrow lexical regression check; supplements inspection and mocks, not a C security proof.
    src=re.sub(r'\\\r?\n','',src)
    src=re.sub(r'/\*.*?\*/|//[^\n]*',' ',src,flags=re.S)
    # Only the observed platform.c scalar assignment and address range/RVA statements are metadata.
    # A matching type/name elsewhere must not exempt callback aliases or invocations in another context.
    metadata_statements=(
        'UINT32 attr=info[k].Attributes;',
        'if((UINTN)binding->Stop>=(UINTN)image->ImageBase&&(UINTN)binding->Stop-(UINTN)image->ImageBase<image->ImageSize)',
        'TextHex(t,(UINTN)binding->Stop-(UINTN)image->ImageBase,16);',
    )
    for statement in metadata_statements:
        src=re.sub(r'\b'+re.escape(statement),
                   lambda match:match[0].replace('.Attributes','.OpenProtocolMetadataAttributes').replace('->Stop','->StopMetadataAddress'),src)
    memory_pattern=(r'(?:->|\.)\s*(?:Io\b|Mem\b(?!\s*\.\s*Read\b))' if allow_mmio_reads
                    else r'(?:->|\.)\s*(?:Mem|Io)\b')
    for pattern in (memory_pattern, r'(?:->|\.)\s*Pci\s*\.\s*Write\b',
                    r'(?:->|\.)\s*(?:Map|Unmap|AllocateBuffer|FreeBuffer|Attributes|SetBarAttributes|PollMem|PollIo|CopyMem)\b',
                    r'(?:->|\.)\s*(?:SetVariable|ResetSystem|DisconnectController|ConnectController|StartImage|LoadImage|UnloadImage|ExitBootServices|CloseProtocol|SetMode|Blt|Stop|Start|Supported)\b'):
        require(not re.search(pattern,src),'Forbidden call: '+pattern)


def validate_capture(capture, build_id):
    require(capture.get('schema')==5 and capture.get('probe_version')=='0.5.0-4070s-readonly-cold'
            and capture.get('probe_mode')=='readonly-cold-4070s','Mock cold profile')
    require(capture.get('build_id')==build_id,'Mock build identity')
    for key in ('gpu_writes','unlock_attempted','hardware_support_validated'):
        require(capture.get(key) is False,'Mock safety flag: '+key)
    require(capture.get('mmio_reads') is True,'Mock observed bounded MMIO')
    require(capture['unique_expected_identity_observed'] is True
            and capture['target_id_count']==capture['exact_identity_count']==1
            and len(capture['display_devices'])==2,'Mock identity')
    target=capture['display_devices'][0]
    require((target['vendor'],target['device_id'],target['subsystem_vendor'],target['subsystem_device'],target['revision'])
            ==('0x10de','0x2783','0x1458','0x413b',0xa1),'Mock exact board')
    config=bytes.fromhex(target['config64'])
    require(len(config)==64 and config[:4]==bytes.fromhex('de108327')
            and config[44:48]==bytes.fromhex('58143b41') and config[8]==0xa1
            and config[11]==3 and config[14]&0x7f==0,'Mock config identity')
    extended=bytes.fromhex(capture['config256'])
    require(len(extended)==256 and extended[:64]==config,'Mock config256')
    full=bytes.fromhex(capture['config4096'])
    require(len(full)==4096 and full[:256]==extended
            and full[64:]==bytes((i*37+11)&255 for i in range(64,4096))
            and capture.get('config4096_status')=='0x0000000000000000','Mock full config binding')
    cached=bytes.fromhex(capture['cached_rom_hex'])
    require(512<=len(cached)<=1024*1024 and len(cached)==target['cached_efi_rom']['size']
            and hashlib.sha256(cached).hexdigest()==target['cached_efi_rom']['sha256'],'Mock cached ROM binding')
    require([g['display_parent_index'] for g in capture['gops']]==[0,1],'Mock GOP ancestry')
    offsets=(0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,
             0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,
             0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100)
    registers=capture['register_snapshot']
    require(registers.get('guard_passed') is True and registers.get('complete') is True
            and registers.get('read_attempts')==23 and registers.get('pci_snapshot_unchanged') is True
            and registers.get('final_pci_status')=='0x0000000000000000'
            and registers.get('final_chip')=='0x194000a1'
            and registers.get('unlock_eligibility') is False,'Mock complete register observation')
    samples=registers['samples']
    require(len(samples)==22,'Mock fixed register count')
    for sample,offset in zip(samples,offsets):
        value=0x194000a1 if offset==0 else offset^0x00010001
        require(sample.get('offset')==f'0x{offset:08x}' and sample.get('value')==f'0x{value:08x}'
                and sample.get('status')=='0x0000000000000000','Mock register value binding')
    prom=capture['physical_prom']
    require(prom.get('guard_passed') is True and prom.get('complete') is True
            and prom.get('identity_stable') is True and prom.get('read_attempts')==259
            and prom.get('offset')=='0x00300000' and prom.get('length')==1024*1024
            and prom.get('data_read_status')=='0x0000000000000000'
            and prom.get('unlock_eligibility') is False,'Mock complete PROM observation')
    raw=bytes.fromhex(prom['hex'])
    require(raw==b'\x55\xaa'+bytes(1024*1024-2)
            and hashlib.sha256(raw).hexdigest()==prom.get('sha256'),'Mock physical PROM bytes/hash')
    platform=capture['platform']
    require(platform['available'] is True and platform['complete'] is True
            and platform['handle_count']==5 and platform['edge_count']==2
            and platform['atomic_snapshot'] is False and platform['release_authorized'] is False,'Mock ownership inventory')
    require(len(platform['gpu_agents'])==1,'Mock driver inventory')
    code=platform['gpu_agents'][0]['code']
    require(code['length']==16 and bytes.fromhex(code['hex'])==bytes(range(16))
            and hashlib.sha256(bytes.fromhex(code['hex'])).hexdigest()==code['loaded_sha256'],'Mock loaded code')


def main():
    root=Path(__file__).resolve().parents[1]
    validate_pe((root/'build/AD104-PROBE.EFI').read_bytes())
    for path in sorted((root/'src').iterdir()):
        if path.suffix in ('.c','.h'):
            validate_source(path.read_text(),allow_mmio_reads=path.name=='sample.c')
    build_id=re.search(r'"([0-9a-f]{64})"',(root/'build/build_id.h').read_text()).group(1)
    for name,length in (('host-capture.json',1536),('host-capture-min-rom.json',512),('host-capture-max-rom.json',1024*1024)):
        capture=json.loads((root/'build'/name).read_text())
        validate_capture(capture,build_id)
        require(len(capture['cached_rom_hex'])==length*2,'Mock actual ROM length: '+name)
    print('PASS: x64 EFI PE; fixed read-only sampler; exact-board cold captures and ownership inventory')


if __name__=='__main__':
    main()
