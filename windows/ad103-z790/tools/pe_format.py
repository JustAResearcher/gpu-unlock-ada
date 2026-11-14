# SPDX-License-Identifier: MIT
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
