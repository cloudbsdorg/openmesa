// Copyright (c) 2026 REVYTECH, Inc.
// SPDX-License-Identifier: BSD-2-Clause
//
// vector_add.cu - cuda-native toolchain test input (Track #2380): c = a + b.

__global__ void vadd(const float *a, const float *b, float *c, int n)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;

	if (i < n)
		c[i] = a[i] + b[i];
}

int launch_vadd(const float *a, const float *b, float *c, int n)
{
	vadd<<<(n + 255) / 256, 256>>>(a, b, c, n);
	return 0;
}
