// FP8 (E4M3) tensor-core issue-rate probe for sm_89.
//
// Same harness as the fork's diagnostics/bench/mma_bench.cu so the number is
// directly comparable with its f16/f32 = 328.9 TF/s peak:
//   BLOCKS=1056, WARPS_PER_BLOCK=8, CHAINS=8 independent accumulators,
//   FLOPs per warp-instruction = 2 * 16 * 8 * 32 = 8192 (same as m16n16k16).
//
//   mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32
//   -> A: 4 x b32, B: 2 x b32, C/D: 4 x f32
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

#define BLOCKS 1056
#define WARPS_PER_BLOCK 8
#define CHAINS 8
#define FP8_FLOPS 8192.0   /* 16*8*32*2 */

__device__ __forceinline__ void mma_fp8(float &d0, float &d1, float &d2, float &d3,
                                        unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                                        unsigned b0, unsigned b1) {
#if __CUDA_ARCH__ >= 890
    asm volatile(
        "mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32 "
        "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
        : "+f"(d0), "+f"(d1), "+f"(d2), "+f"(d3)
        : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
#endif
}

__global__ void bench_fp8(unsigned long long iters, float *sink) {
    float acc[CHAINS][4];
    unsigned a[4], b[2];
    /* 0x38 == 1.0 in E4M3, so every product is 1.0 */
    const unsigned one = 0x38383838u;
#pragma unroll
    for (int c = 0; c < CHAINS; c++) { acc[c][0]=acc[c][1]=acc[c][2]=acc[c][3]=0.f; }
#pragma unroll
    for (int i = 0; i < 4; i++) a[i] = one;
    b[0] = one; b[1] = one;

    for (unsigned long long it = 0; it < iters; it++) {
#pragma unroll
        for (int c = 0; c < CHAINS; c++)
            mma_fp8(acc[c][0], acc[c][1], acc[c][2], acc[c][3], a[0], a[1], a[2], a[3], b[0], b[1]);
    }
    float s = 0.f;
#pragma unroll
    for (int c = 0; c < CHAINS; c++) s += acc[c][0] + acc[c][1] + acc[c][2] + acc[c][3];
    if (threadIdx.x == 0 && blockIdx.x == 0) sink[0] = s;
}

/* one instruction, all-ones inputs: each output element must be k = 32 */
__global__ void probe_fp8(float *out) {
    float d0 = 0.f, d1 = 0.f, d2 = 0.f, d3 = 0.f;
    const unsigned one = 0x38383838u;
    mma_fp8(d0, d1, d2, d3, one, one, one, one, one, one);
    if (threadIdx.x == 0) { out[0] = d0; out[1] = d1; out[2] = d2; out[3] = d3; }
}

int main(int argc, char **argv) {
    unsigned long long iters = argc > 1 ? strtoull(argv[1], 0, 10) : 20000ULL;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    float *sink, *out;
    cudaMalloc(&sink, sizeof(float));
    cudaMalloc(&out, 4 * sizeof(float));

    probe_fp8<<<1, 32>>>(out);
    cudaDeviceSynchronize();
    float h[4];
    cudaMemcpy(h, out, sizeof(h), cudaMemcpyDeviceToHost);
    printf("# instruction check: d = %.1f %.1f %.1f %.1f (expect 32.0 each)\n", h[0], h[1], h[2], h[3]);
    if (h[0] != 32.0f) { printf("FAIL: FP8 mma produced wrong value\n"); return 2; }

    double warps = (double)BLOCKS * WARPS_PER_BLOCK;
    double best = 0.0;
    for (int r = 0; r < reps; r++) {
        cudaEvent_t s, e; cudaEventCreate(&s); cudaEventCreate(&e);
        cudaEventRecord(s);
        bench_fp8<<<BLOCKS, WARPS_PER_BLOCK * 32>>>(iters, sink);
        cudaEventRecord(e);
        cudaEventSynchronize(e);
        float ms = 0; cudaEventElapsedTime(&ms, s, e);
        double tflops = warps * (double)iters * (FP8_FLOPS * CHAINS) / (ms * 1e9);
        printf("  rep %d: %.3f ms  %.1f TF/s\n", r, ms, tflops);
        if (tflops > best) best = tflops;
        cudaEventDestroy(s); cudaEventDestroy(e);
    }
    printf("e4m3/e4m3->f32  best %.1f TF/s   (reference: f16/f32 328.9, f16/f16 330.8, s8/s32 657.7)\n", best);
    return 0;
}
