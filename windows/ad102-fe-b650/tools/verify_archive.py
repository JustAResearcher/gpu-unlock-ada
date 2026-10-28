"""Verify freestanding COFF archive and the documented external logging API."""
from pathlib import Path
import subprocess,re,json
root=Path(__file__).resolve().parents[1]
archive=root/'build/AD102-TRANSACTION.a'
result=subprocess.run(['nm','-g',str(archive)],capture_output=True,text=True,check=True).stdout
defined=set();undefined=set()
for line in result.splitlines():
    m=re.search(r'(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(\S+)$',line)
    if not m:continue
    kind,symbol=m.groups()
    (undefined if kind=='U' else defined).add(symbol)
external=sorted(undefined-defined)
assert external==['GuLog'],external
assert 'efi_main' not in defined and 'UefiMain' not in defined
stack=[]
for p in (root/'build').glob('*.su'):
    for line in p.read_text().splitlines():
        loc,used,kind=line.rsplit('\t',2)
        assert int(used)<=4096,(loc,used,kind)
        stack.append(int(used))
assert stack
out={'archive':archive.name,'unresolved':external,'max_reported_function_stack':max(stack),'has_efi_entry':False,'hardware_test':False}
(root/'build/archive-verification.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out))
