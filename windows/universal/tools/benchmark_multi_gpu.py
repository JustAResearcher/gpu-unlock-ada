import sys
import subprocess
import argparse
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

def get_clean_name(name):
    return name.replace('NVIDIA GeForce ', '').replace('NVIDIA ', '').replace('GeForce ', '').strip()

def benchmark_gemm(dev_idx, dtype, dtype_name, sizes=[2048, 4096, 8192], iters=15, warmup=5):
    results = {}
    with torch.cuda.device(dev_idx):
        for N in sizes:
            try:
                a = torch.randn(N, N, device=f'cuda:{dev_idx}', dtype=dtype)
                b = torch.randn(N, N, device=f'cuda:{dev_idx}', dtype=dtype)
                
                # Warmup
                for _ in range(warmup):
                    c = torch.matmul(a, b)
                torch.cuda.synchronize(dev_idx)
                
                telem = get_telemetry(dev_idx)
                
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
                
                results[N] = {
                    'ms': elapsed_ms,
                    'tflops': tflops,
                    'telem': telem
                }
                del a, b, c
                torch.cuda.empty_cache()
            except RuntimeError as e:
                # Catch OOM if matrix is too large for smaller VRAM
                results[N] = {'ms': 0.0, 'tflops': 0.0, 'telem': get_telemetry(dev_idx), 'error': 'OOM'}
                torch.cuda.empty_cache()
    return results

def main():
    parser = argparse.ArgumentParser(description="Universal Multi-GPU Matrix Multiplication Benchmark (PyTorch)")
    parser.add_argument("--sizes", type=str, default="2048,4096,8192", help="Comma-separated matrix sizes (e.g. 2048,4096,8192)")
    parser.add_argument("--iters", type=int, default=15, help="Number of benchmark iterations")
    args = parser.parse_args()

    if not torch.cuda.is_available():
        print("Error: CUDA is not available on this system.")
        return

    sizes = [int(s.strip()) for s in args.sizes.split(",") if s.strip().isdigit()]
    count = torch.cuda.device_count()

    print("=" * 95)
    print("      UNIVERSAL MULTI-GPU PERFORMANCE BENCHMARK (PyTorch CUDA cuBLAS GEMM)")
    print("=" * 95)
    
    devices = []
    for i in range(count):
        full_name = torch.cuda.get_device_name(i)
        props = torch.cuda.get_device_properties(i)
        clean_name = get_clean_name(full_name)
        vram = props.total_memory / (1024 ** 3)
        devices.append({
            'idx': i,
            'full_name': full_name,
            'clean_name': clean_name,
            'vram': vram,
            'sms': props.multi_processor_count,
            'cc': (props.major, props.minor)
        })
        print(f" [GPU {i}] {clean_name:<25} | {vram:>6.2f} GB VRAM | {props.multi_processor_count:>3} SMs | Compute {props.major}.{props.minor}")
    print("=" * 95)

    gpus_data = []
    for d in devices:
        dev_idx = d['idx']
        print(f"\n>>> Benchmarking GPU {dev_idx}: {d['clean_name']} ...")
        
        # Test BF16 if supported (CC >= 8.0)
        has_bf16 = (d['cc'][0] >= 8)
        bf16_res = benchmark_gemm(dev_idx, torch.bfloat16, "BF16", sizes=sizes, iters=args.iters) if has_bf16 else None
        fp16_res = benchmark_gemm(dev_idx, torch.float16, "FP16", sizes=sizes, iters=args.iters)
        fp32_res = benchmark_gemm(dev_idx, torch.float32, "FP32", sizes=sizes, iters=args.iters)
        
        gpus_data.append({
            'info': d,
            'bf16': bf16_res,
            'fp16': fp16_res,
            'fp32': fp32_res
        })

    # Individual GPU Summaries
    for g in gpus_data:
        d = g['info']
        print(f"\n--- [GPU {d['idx']}: {d['clean_name']}] Individual Results ---")
        print(f"{'Precision':<10} | {'Matrix Size':<12} | {'Throughput (TFLOPS)':<20} | {'Latency (ms)':<14} | {'SM Clock':<10} | {'Power (W)':<10}")
        print("-" * 85)
        for prec_name, key in [("BF16", "bf16"), ("FP16", "fp16"), ("FP32", "fp32")]:
            if g[key] is None:
                print(f"{prec_name:<10} | {'All':<12} | {'N/A (Requires CC >= 8.0)':<20} | {'-':<14} | {'-':<10} | {'-':<10}")
                continue
            for N in sizes:
                entry = g[key].get(N, {})
                if 'error' in entry:
                    print(f"{prec_name:<10} | {N}x{N:<6} | {'OOM (Out of Memory)':<20} | {'-':<14} | {'-':<10} | {'-':<10}")
                else:
                    telem = entry['telem']
                    print(f"{prec_name:<10} | {N}x{N:<6} | {entry['tflops']:>17.2f} TFLOPS | {entry['ms']:>10.3f} ms   | {telem['sm_clock']:>4} MHz   | {telem['power']:>5.1f} W")
        print("-" * 85)

    # Multi-GPU Comparison Matrix
    if count >= 2:
        print("\n" + "=" * 95)
        print("                  DYNAMIC MULTI-GPU PERFORMANCE COMPARISON TABLE")
        print("=" * 95)
        
        # Build dynamic header
        col_headers = [f"GPU {d['idx']}: {d['clean_name']}" for d in devices]
        hdr_str = f"{'Precision':<10} | {'Matrix Size':<12} | " + " | ".join(f"{h:<20}" for h in col_headers)
        if count == 2:
            hdr_str += f" | {'Speedup Ratio':<15}"
        print(hdr_str)
        print("-" * len(hdr_str))

        for prec_name, key in [("BF16", "bf16"), ("FP16", "fp16"), ("FP32", "fp32")]:
            for N in sizes:
                row = [f"{prec_name:<10}", f"{N}x{N:<6}"]
                tflops_vals = []
                for g in gpus_data:
                    if g[key] is None or N not in g[key] or 'error' in g[key][N]:
                        row.append(f"{'N/A':<20}")
                        tflops_vals.append(0.0)
                    else:
                        tf = g[key][N]['tflops']
                        row.append(f"{tf:>13.2f} TFLOPS")
                        tflops_vals.append(tf)
                
                # If exactly 2 GPUs, add ratio column
                if count == 2:
                    if tflops_vals[1] > 0 and tflops_vals[0] > 0:
                        ratio = tflops_vals[0] / tflops_vals[1]
                        if ratio >= 1.0:
                            ratio_str = f"GPU 0 is {ratio:.2f}x"
                        else:
                            ratio_str = f"GPU 1 is {(1.0 / ratio):.2f}x"
                    else:
                        ratio_str = "-"
                    row.append(f"{ratio_str:<15}")
                
                print(" | ".join(row))
            print("-" * len(hdr_str))
        print("=" * len(hdr_str))

if __name__ == '__main__':
    main()
