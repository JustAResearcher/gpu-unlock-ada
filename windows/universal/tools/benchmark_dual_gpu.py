import time
import subprocess
import torch

def get_telemetry(dev_idx):
    try:
        fields = 'clocks.current.sm,clocks.current.memory,temperature.gpu,power.draw'
        res = subprocess.run(
            ['nvidia-smi', f'--id={dev_idx}', f'--query-gpu={fields}', '--format=csv,noheader,nounits'],
            capture_output=True, text=True, check=True
        )
        sm, mem, temp, pwr = res.stdout.strip().split(',')
        return {
            'sm_clock': int(sm.strip()),
            'mem_clock': int(mem.strip()),
            'temp': int(temp.strip()),
            'power': float(pwr.strip())
        }
    except Exception:
        return {'sm_clock': 0, 'mem_clock': 0, 'temp': 0, 'power': 0.0}

def benchmark_gemm(dev_idx, dtype, dtype_name, sizes=[2048, 4096, 8192], iters=20, warmup=5):
    results = []
    with torch.cuda.device(dev_idx):
        for N in sizes:
            a = torch.randn(N, N, device=f'cuda:{dev_idx}', dtype=dtype)
            b = torch.randn(N, N, device=f'cuda:{dev_idx}', dtype=dtype)
            
            # Warmup
            for _ in range(warmup):
                c = torch.matmul(a, b)
            torch.cuda.synchronize()
            telem = get_telemetry(dev_idx)
            
            # Precise timing
            start = torch.cuda.Event(enable_timing=True)
            end = torch.cuda.Event(enable_timing=True)
            
            start.record()
            for _ in range(iters):
                c = torch.matmul(a, b)
            end.record()
            end.synchronize()
            
            elapsed_ms = start.elapsed_time(end) / iters
            total_flops = 2.0 * (N ** 3)
            tflops = (total_flops / (elapsed_ms * 1e-3)) / 1e12
            
            results.append({
                'size': N,
                'ms': elapsed_ms,
                'tflops': tflops,
                'telem': telem
            })
            del a, b, c
            torch.cuda.empty_cache()
    return results

def main():
    if not torch.cuda.is_available():
        print("CUDA is not available!")
        return

    count = torch.cuda.device_count()
    print("=" * 85)
    print("           DUAL-GPU BF16 / FP16 / FP32 MATRIX BENCHMARK (PyTorch CUDA)")
    print("=" * 85)
    for i in range(count):
        name = torch.cuda.get_device_name(i)
        props = torch.cuda.get_device_properties(i)
        vram = props.total_memory / (1024 ** 3)
        print(f" [GPU {i}] {name:<30} | {vram:.2f} GB VRAM | {props.multi_processor_count} SMs | CC {props.major}.{props.minor}")
    print("=" * 85)

    gpus_data = []
    for dev_idx in range(count):
        dev_name = torch.cuda.get_device_name(dev_idx)
        print(f"\n>>> Running benchmarks on GPU {dev_idx}: {dev_name} ...")
        
        bf16_res = benchmark_gemm(dev_idx, torch.bfloat16, "BF16")
        fp16_res = benchmark_gemm(dev_idx, torch.float16, "FP16")
        fp32_res = benchmark_gemm(dev_idx, torch.float32, "FP32")
        
        gpus_data.append({
            'idx': dev_idx,
            'name': dev_name,
            'bf16': bf16_res,
            'fp16': fp16_res,
            'fp32': fp32_res
        })

    # Summary Table Comparison
    print("\n" + "=" * 85)
    print("                    DIRECT COMPARISON (4096^3 & 8192^3 GEMM)")
    print("=" * 85)
    header = f"{'Precision':<10} | {'Matrix Size':<12} | {'RTX 3060 (TFLOPS)':<18} | {'RTX 4070 Ti (TFLOPS)':<20} | {'Ratio (4070/3060)':<18}"
    print(header)
    print("-" * 85)

    gpu3060 = next((g for g in gpus_data if '3060' in g['name']), gpus_data[-1] if len(gpus_data) > 1 else None)
    gpu4070 = next((g for g in gpus_data if '4070' in g['name']), gpus_data[0] if len(gpus_data) > 0 else None)

    if gpu3060 and gpu4070:
        for precision, key in [('BF16', 'bf16'), ('FP16', 'fp16'), ('FP32', 'fp32')]:
            for size_idx in range(len(gpu3060[key])):
                size = gpu3060[key][size_idx]['size']
                t3060 = gpu3060[key][size_idx]['tflops']
                t4070 = gpu4070[key][size_idx]['tflops']
                ratio = t4070 / t3060 if t3060 > 0 else 0.0
                print(f"{precision:<10} | {size}x{size:<6} | {t3060:>10.2f} TFLOPS   | {t4070:>12.2f} TFLOPS     | {ratio:>6.2f}x faster")
            print("-" * 85)

    print("=" * 85)

if __name__ == '__main__':
    main()

