from pathlib import Path
import importlib.util,json,struct
r=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pecheck',r/'tools/pe_format.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p=(r/'build/AD104-EXPERIMENTAL.EFI').read_bytes();m.validate_pe(p)
pe=struct.unpack_from('<I',p,60)[0];opt=pe+24
base=struct.unpack_from('<Q',p,opt+24)[0]
assert base==0x180000000
max_stack=0
for f in (r/'build').glob('*.su'):
 for line in f.read_text().splitlines():
  _,size,kind=line.rsplit('\t',2);assert int(size)<=4096,(line,f);max_stack=max(max_stack,int(size))
out={'x64_efi_verified':True,'imports_tls_clr':False,'image_base':hex(base),'max_function_stack':max_stack,'hardware_tested':False,'gpu_write_capability':True}
(r/'build/experimental-pe.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
