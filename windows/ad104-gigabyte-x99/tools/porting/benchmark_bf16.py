#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Бенчмарк производительности Tensor Core (BF16, FP16) и CUDA Core (FP32)
для валидации разблокировки GPU в Windows.
"""
import time
import subprocess
import torch

def get_telemetry():
    try:
        fields = 'clocks.current.sm,clocks.current.memory,temperature.gpu,power.draw'
        res = subprocess.run(
            ['nvidia-smi', f'--query-gpu={fields}', '--format=csv,noheader,nounits'],
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

def benchmark_gemm(dtype, dtype_name, sizes=[2048, 4096, 8192, 16384], iters=20, warmup=10):
    print(f"\n========================================================")
    print(f"  Тестирование: {dtype_name.upper()} (GEMM)")
    print(f"========================================================")
    print(f"{'Размер матрицы':<15} | {'Задержка (мс)':<15} | {'Скорость (TFLOPS)':<20} | {'Частота SM':<12} | {'Мощность':<10}")
    print("-" * 80)
    
    results = []
    for N in sizes:
        a = torch.randn(N, N, device='cuda', dtype=dtype)
        b = torch.randn(N, N, device='cuda', dtype=dtype)
        
        # Прогрев кэша
        for _ in range(warmup):
            c = torch.matmul(a, b)
        torch.cuda.synchronize()
        
        # Точный замер через CUDA Events
        start = torch.cuda.Event(enable_timing=True)
        end = torch.cuda.Event(enable_timing=True)
        
        start.record()
        for _ in range(iters):
            c = torch.matmul(a, b)
        end.record()
        end.synchronize()
        
        telem = get_telemetry()
        elapsed_ms = start.elapsed_time(end) / iters
        total_flops = 2.0 * (N ** 3)
        tflops = (total_flops / (elapsed_ms * 1e-3)) / 1e12
        
        results.append({
            'size': N,
            'ms': elapsed_ms,
            'tflops': tflops,
            'telem': telem
        })
        
        print(f"{N}x{N:<10} | {elapsed_ms:<15.3f} | {tflops:<20.2f} | {telem['sm_clock']:<5} MHz   | {telem['power']:<5.1f} W")
    
    return results

def main():
    if not torch.cuda.is_available():
        print("Ошибка: CUDA недоступна в PyTorch!")
        return

    dev_name = torch.cuda.get_device_name(0)
    total_mem = torch.cuda.get_device_properties(0).total_memory / (1024 ** 3)
    sm_count = torch.cuda.get_device_properties(0).multi_processor_count
    
    print("========================================================")
    print("       БЕНЧМАРК ПРОИЗВОДИТЕЛЬНОСТИ NVIDIA GPU (PyTorch)")
    print("========================================================")
    print(f"Видеокарта:       {dev_name}")
    print(f"Видеопамять:      {total_mem:.2f} GB")
    print(f"SM блоков:        {sm_count} SMs")
    print(f"PyTorch версия:   {torch.__version__}")
    print(f"CUDA Runtime:     {torch.version.cuda}")
    print("========================================================")
    
    # 1. BF16
    bf16_res = benchmark_gemm(torch.bfloat16, "BF16 (Bfloat16 Tensor Cores)")
    
    # 2. FP16
    fp16_res = benchmark_gemm(torch.float16, "FP16 (Half Precision Tensor Cores)")
    
    # 3. FP32
    fp32_res = benchmark_gemm(torch.float32, "FP32 (Single Precision CUDA Cores)")
    
    print("\n========================================================")
    print("                      ИТОГОВЫЙ СРЕЗ")
    print("========================================================")
    max_bf16 = max(r['tflops'] for r in bf16_res)
    max_fp16 = max(r['tflops'] for r in fp16_res)
    max_fp32 = max(r['tflops'] for r in fp32_res)
    
    print(f"Максимальный пик BF16:  {max_bf16:.2f} TFLOPS")
    print(f"Максимальный пик FP16:  {max_fp16:.2f} TFLOPS")
    print(f"Максимальный пик FP32:  {max_fp32:.2f} TFLOPS")
    print("========================================================")

if __name__ == "__main__":
    main()
