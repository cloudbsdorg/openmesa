// Copyright (c) 2026 REVYTECH, Inc.
// SPDX-License-Identifier: MIT
//
// reduce_sum.cu - libnvptx test input (Track #2426): per-block sum of n
// floats through shared memory and __syncthreads, then a warp shuffle
// reduction of the last 32 partial sums.

__global__ void reduce_sum(const float *in, float *out, int n)
{
	__shared__ float part[256];
	unsigned int t = threadIdx.x;
	unsigned int i = blockIdx.x * blockDim.x + t;

	part[t] = i < (unsigned int)n ? in[i] : 0.0f;
	__syncthreads();
	for (unsigned int s = blockDim.x / 2; s >= 32; s >>= 1) {
		if (t < s)
			part[t] += part[t + s];
		__syncthreads();
	}
	if (t < 32) {
		float v = part[t];
		for (int o = 16; o > 0; o >>= 1)
			v += __shfl_down_sync(0xffffffffu, v, o);
		if (t == 0)
			out[blockIdx.x] = v;
	}
}

__global__ void reduce_max_i32(const int *in, int *out, int n)
{
	int i = blockIdx.x * blockDim.x + threadIdx.x;

	if (i < n)
		__nvvm_atom_max_gen_i(out, in[i]);
}
