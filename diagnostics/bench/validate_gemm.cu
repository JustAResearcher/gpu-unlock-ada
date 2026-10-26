// SPDX-License-Identifier: GPL-2.0-only
/*
 * Deterministic numerical check for Ada FP16/BF16 tensor GEMM with FP32
 * accumulation. Build with CUDA 12.8 and run before and after the unlock on
 * the same exact GPU. This is a correctness check, not a throughput benchmark.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cublas_v2.h>
#include "gemm_inputs.h"

static void cuda_check(cudaError_t code, const char *where)
{
	if (code != cudaSuccess) {
		std::fprintf(stderr, "%s: %s\n", where, cudaGetErrorString(code));
		std::exit(2);
	}
}

static void blas_check(cublasStatus_t code, const char *where)
{
	if (code != CUBLAS_STATUS_SUCCESS) {
		std::fprintf(stderr, "%s: cuBLAS status %d\n", where, (int)code);
		std::exit(2);
	}
}

template <typename T>
static T convert(float x);
template <> __half convert<__half>(float x) { return __float2half_rn(x); }
template <> __nv_bfloat16 convert<__nv_bfloat16>(float x)
{
	return __float2bfloat16_rn(x);
}

template <typename T>
static bool check_mode(cublasHandle_t handle, const char *name,
			    cudaDataType input_type, int m, int n, int k, uint32_t seed)
{
	const size_t count = (size_t)m * n;
	std::vector<T> a((size_t)m * k), b((size_t)k * n);
	std::vector<float> reference(count, 0.0f), result(count);
	T *device_a = nullptr, *device_b = nullptr;
	float *device_c = nullptr;
	const float alpha = 1.0f, beta = 0.0f;
	int failures = 0;
	float max_abs = 0.0f;

	for (int col = 0; col < k; ++col)
		for (int row = 0; row < m; ++row)
			a[row + col * m] = convert<T>(gemm_input(row, col, seed));
	for (int col = 0; col < n; ++col)
		for (int row = 0; row < k; ++row)
			b[row + col * k] = convert<T>(gemm_input(row, col, seed ^ 0xa5a5a5a5u));
	for (int col = 0; col < n; ++col)
		for (int inner = 0; inner < k; ++inner)
			for (int row = 0; row < m; ++row)
				reference[row + col * m] +=
					(float)a[row + inner * m] * (float)b[inner + col * k];

	cuda_check(cudaMalloc(&device_a, a.size() * sizeof(T)), "cudaMalloc(A)");
	cuda_check(cudaMalloc(&device_b, b.size() * sizeof(T)), "cudaMalloc(B)");
	cuda_check(cudaMalloc(&device_c, count * sizeof(float)), "cudaMalloc(C)");
	cuda_check(cudaMemcpy(device_a, a.data(), a.size() * sizeof(T), cudaMemcpyHostToDevice), "copy A");
	cuda_check(cudaMemcpy(device_b, b.data(), b.size() * sizeof(T), cudaMemcpyHostToDevice), "copy B");
	blas_check(cublasGemmEx(handle, CUBLAS_OP_N, CUBLAS_OP_N, m, n, k,
				&alpha, device_a, input_type, m, device_b, input_type, k,
				&beta, device_c, CUDA_R_32F, m,
				CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT_TENSOR_OP), "cublasGemmEx");
	cuda_check(cudaDeviceSynchronize(), "GEMM synchronize");
	cuda_check(cudaMemcpy(result.data(), device_c, count * sizeof(float),
			      cudaMemcpyDeviceToHost), "copy C");
	for (size_t i = 0; i < count; ++i) {
		float diff = std::fabs(result[i] - reference[i]);
		if (!std::isfinite(result[i]) || diff > 1e-3f) {
			if (++failures <= 4)
				std::fprintf(stderr, "%s mismatch [%zu]: GPU %.8g, CPU %.8g\n",
					     name, i, result[i], reference[i]);
		}
		if (diff > max_abs)
			max_abs = diff;
	}
	std::printf("%s: m=%d n=%d k=%d seed=%u, %zu cells, failures=%d, max_abs=%.8g\n",
		    name, m, n, k, seed, count, failures, max_abs);
	cuda_check(cudaFree(device_a), "free A");
	cuda_check(cudaFree(device_b), "free B");
	cuda_check(cudaFree(device_c), "free C");
	return failures == 0;
}

int main()
{
	cublasHandle_t handle;
	cudaDeviceProp prop;
	char bus_id[32];
	cuda_check(cudaGetDeviceProperties(&prop, 0), "device properties");
	cuda_check(cudaDeviceGetPCIBusId(bus_id, sizeof(bus_id), 0), "PCI bus ID");
	std::printf("GPU: %s, sm_%d%d, PCI %s\n", prop.name, prop.major,
		    prop.minor, bus_id);
	if (prop.major != 8 || (prop.minor != 6 && prop.minor != 9)) {
		std::fprintf(stderr, "Expected an Ampere sm_86 or Ada sm_89 device\n");
		return 3;
	}
	blas_check(cublasCreate(&handle), "cublasCreate");
	bool pass = true;
	const struct { int m, n, k; uint32_t seed; } cases[] = {
		{256, 256, 256, 1}, {64, 96, 128, 2}, {127, 65, 95, 3},
	};
	for (const auto &test : cases) {
		pass &= check_mode<__half>(handle, "FP16/FP32", CUDA_R_16F,
					 test.m, test.n, test.k, test.seed);
		pass &= check_mode<__nv_bfloat16>(handle, "BF16/FP32", CUDA_R_16BF,
						 test.m, test.n, test.k, test.seed);
	}
	blas_check(cublasDestroy(handle), "cublasDestroy");
	return pass ? 0 : 1;
}
