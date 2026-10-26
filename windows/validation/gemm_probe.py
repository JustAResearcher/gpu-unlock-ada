# SPDX-License-Identifier: GPL-2.0-only
"""Bounded ordinary CUDA computation; no register, driver or power-setting writes."""
from pathlib import Path
import argparse,datetime,hashlib,json,os,statistics,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--label',required=True,choices=['stock','trial','restored']);args=p.parse_args()
root=Path(__file__).parent; (root/'local').mkdir(exist_ok=True);stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S');output=root/'local'/f'windows-compute-{args.label}-{stamp}.json'
def query(fields):
 s=subprocess.check_output(['nvidia-smi','--query-gpu='+fields,'--format=csv,noheader,nounits'],text=True,timeout=10).strip();return [v.strip() for v in s.split(',')]
def telemetry():return dict(zip(['temperature_c','sm_clock_mhz','memory_clock_mhz','power_w','pstate','utilization_percent'],query('temperature.gpu,clocks.sm,clocks.mem,power.draw,pstate,utilization.gpu')))
lock=root/'local/windows-compute.lock';handle=lock.open('x');handle.write(str(os.getpid()));handle.close()
report={'label':args.label,'utc':stamp,'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'hardware_settings_changed':False,'results':[],'complete':False,'scope':'Dense matrix multiply proxy with sampled FP64 correctness reference; not LLM throughput or sustained qualification.'}
try:
 pre=[query('utilization.gpu,memory.free,temperature.gpu') for _ in range(3)]
 if any(float(v[0])>5 or float(v[1])<3000 or float(v[2])>65 for v in pre):raise RuntimeError('GPU busy, insufficient memory, or too warm; nothing stopped.')
 report['identity']=query('name,pci.bus_id,driver_version,vbios_version,memory.total')
 if len(subprocess.check_output(['nvidia-smi','--query-gpu=name','--format=csv,noheader'],text=True,timeout=10).splitlines())!=1:raise RuntimeError('This collector requires exactly one NVIDIA GPU')
 if 'CUDA_VISIBLE_DEVICES' in os.environ:raise RuntimeError('Run without CUDA_VISIBLE_DEVICES remapping')
 import torch
 if torch.cuda.device_count()!=1:raise RuntimeError('Exactly one CUDA device is required')
 if hasattr(torch.backends.cuda.matmul,'allow_fp16_accumulation'):torch.backends.cuda.matmul.allow_fp16_accumulation=False
 torch.set_num_threads(4);torch.manual_seed(4070);torch.cuda.manual_seed_all(4070)
 report.update(torch_version=torch.__version__,cuda_version=torch.version.cuda,device_capability=torch.cuda.get_device_capability(0),initial_telemetry=telemetry())
 torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction=False
 torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction=False
 # Fixed data and order, three timing batches, no autotuning or model/service changes.
 n=4096;begin=time.monotonic()
 with torch.inference_mode():
  for name,dtype,tf32,atol,rtol in [('fp16',torch.float16,False,.003,.003),('bf16',torch.bfloat16,False,.025,.025),('tf32',torch.float32,True,.005,.005),('fp32_control',torch.float32,False,.00005,.00005),('fp8_e4m3',torch.float8_e4m3fn,False,.003,.003)]:
   torch.backends.cuda.matmul.allow_tf32=tf32
   fp8=name.startswith('fp8')
   if fp8:
    # Ada native scaled FP8 GEMM: column-major B, FP32 scales, FP16 output.
    # Quantization/conversion is outside the timing; reference uses quantized inputs.
    a=(torch.randn((n,n),device='cuda',dtype=torch.float32)/n**.5).to(dtype)
    b=torch.randn((n,n),device='cuda',dtype=torch.float32).to(dtype).t()
    scale=torch.ones((),device='cuda',dtype=torch.float32)
    def multiply():return torch._scaled_mm(a,b,scale_a=scale,scale_b=scale,out_dtype=torch.float16,use_fast_accum=False)
   else:
    a=torch.randn((n,n),device='cuda',dtype=dtype)/n**.5;b=torch.randn((n,n),device='cuda',dtype=dtype);c=torch.empty((n,n),device='cuda',dtype=dtype)
    def multiply():return torch.mm(a,b,out=c)
   for _ in range(10):c=multiply()
   torch.cuda.synchronize();samples=[];observations=[]
   for _ in range(3):
    before=telemetry();start=torch.cuda.Event(enable_timing=True);end=torch.cuda.Event(enable_timing=True);start.record()
    for __ in range(40):c=multiply()
    end.record();end.synchronize();samples.append(start.elapsed_time(end)/40);observations.append({'before':before,'after':telemetry()})
   row=torch.arange(0,n,n//32,device='cuda');col=torch.arange(7,n,n//32,device='cuda')
   ar=(a.float() if fp8 else a).index_select(0,row).double().cpu();bc=(b.float() if fp8 else b).index_select(1,col).double().cpu();ref=ar@bc;actual=c.index_select(0,row).index_select(1,col).double().cpu()
   finite=bool(torch.isfinite(c).all().item());error=(actual-ref).abs();ok=finite and bool((error<=atol+rtol*ref.abs()).all().item())
   report['results'].append(dict(name=name,n=n,input_dtype=str(dtype),output_dtype=str(c.dtype),kernel='aten._scaled_mm / cuBLASLt' if fp8 else 'aten.mm',quantization_timed=False,reference='FP64 product of actual quantized inputs',repetitions=40,batches=3,ms_per_matmul=samples,median_ms=statistics.median(samples),dense_tflops=2*n**3/(statistics.median(samples)*1e9),sample_count=actual.numel(),finite=finite,correctness_passed=ok,max_abs_error=error.max().item(),rms_error=error.square().mean().sqrt().item(),atol=atol,rtol=rtol,telemetry=observations))
   if not ok:raise RuntimeError('Numerical check failed: '+name)
   if time.monotonic()-begin>75:raise RuntimeError('Bounded runtime exceeded')
   del a,b,c,ar,bc,ref,actual;torch.cuda.empty_cache()
 report.update(complete=True,elapsed_seconds=time.monotonic()-begin,final_telemetry=telemetry())
except Exception as e:
 report['error']=str(e)
finally:
 output.write_text(json.dumps(report,indent=2)+'\n');lock.unlink()
print(json.dumps({'output':str(output),'complete':report['complete'],'results':[{k:v for k,v in q.items() if k in ('name','median_ms','dense_tflops','correctness_passed')} for q in report['results']],'error':report.get('error')},indent=2))
if not report['complete']:raise SystemExit(1)
