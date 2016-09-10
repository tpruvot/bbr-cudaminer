extern "C" {
#include "miner.h"
}

static cudaStream_t *scr_copy_streams;
static ulonglong4 **d_scratchpad;
static uint64_t **d_input;
static uint32_t **d_retnonce;

extern unsigned int CUDABlocks, CUDAThreads;

#define st0 	vst0.x
#define st1 	vst0.y
#define st2 	vst0.z
#define st3 	vst0.w

#define st4 	vst4.x
#define st5 	vst4.y
#define st6 	vst4.z
#define st7 	vst4.w

#define st8 	vst8.x
#define st9 	vst8.y
#define st10	vst8.z
#define st11	vst8.w

#define st12	vst12.x
#define st13	vst12.y
#define st14	vst12.z
#define st15	vst12.w

#define st16	vst16.x
#define st17	vst16.y
#define st18	vst16.z
#define st19	vst16.w

#define st20	vst20.x
#define st21	vst20.y
#define st22	vst20.z
#define st23	vst20.w

__noinline__ __device__ uint64_t bitselect(const uint64_t a, const uint64_t b, const uint64_t c) { return(a ^ (c & (b ^ a))); }
__noinline__ __device__ uint64_t cuda_rotl641(const uint64_t x) { return((x << 1) | (x >> 63)); }

#if __CUDA_ARCH__ >= 320
__device__ __forceinline__ uint64_t cuda_rotl64(const uint64_t value, const int offset)
{
	uint2 result;
	if(offset >= 32) {
		asm("shf.l.wrap.b32 %0, %1, %2, %3;"
			: "=r"(result.x) : "r"(__double2loint(__longlong_as_double(value))), "r"(__double2hiint(__longlong_as_double(value))), "r"(offset));
		asm("shf.l.wrap.b32 %0, %1, %2, %3;"
			: "=r"(result.y) : "r"(__double2hiint(__longlong_as_double(value))), "r"(__double2loint(__longlong_as_double(value))), "r"(offset));
	} else {
		asm("shf.l.wrap.b32 %0, %1, %2, %3;"
			: "=r"(result.x) : "r"(__double2hiint(__longlong_as_double(value))), "r"(__double2loint(__longlong_as_double(value))), "r"(offset));
		asm("shf.l.wrap.b32 %0, %1, %2, %3;"
			: "=r"(result.y) : "r"(__double2loint(__longlong_as_double(value))), "r"(__double2hiint(__longlong_as_double(value))), "r"(offset));
	}
	return __double_as_longlong(__hiloint2double(result.y, result.x));
}
#else
__noinline__ __device__ uint64_t cuda_rotl64(const uint64_t x, const uint8_t y) { return((x << y) | (x >> (64 - y))); }
#endif

#define ROTL64(x, y) (cuda_rotl64((x), (y)))
#define ROTL641(x) (cuda_rotl641(x))

#define RND() \
	bc[0] = st0 ^ st5 ^ st10 * st15 * st20 ^ ROTL641(st2 ^ st7 ^ st12 * st17 * st22); \
	bc[1] = st1 ^ st6 ^ st11 * st16 * st21 ^ ROTL641(st3 ^ st8 ^ st13 * st18 * st23); \
	bc[2] = st2 ^ st7 ^ st12 * st17 * st22 ^ ROTL641(st4 ^ st9 ^ st14 * st19 * st24); \
	bc[3] = st3 ^ st8 ^ st13 * st18 * st23 ^ ROTL641(st0 ^ st5 ^ st10 * st15 * st20); \
	bc[4] = st4 ^ st9 ^ st14 * st19 * st24 ^ ROTL641(st1 ^ st6 ^ st11 * st16 * st21); \
	tmp1 = st1 ^ bc[0]; \
	\
	st0 ^= bc[4]; \
	st1 = ROTL64(st6 ^ bc[0], 44); \
	st6 = ROTL64(st9 ^ bc[3], 20); \
	st9 = ROTL64(st22 ^ bc[1], 61); \
	st22 = ROTL64(st14 ^ bc[3], 39); \
	st14 = ROTL64(st20 ^ bc[4], 18); \
	st20 = ROTL64(st2 ^ bc[1], 62); \
	st2 = ROTL64(st12 ^ bc[1], 43); \
	st12 = ROTL64(st13 ^ bc[2], 25); \
	st13 = ROTL64(st19 ^ bc[3], 8); \
	st19 = ROTL64(st23 ^ bc[2], 56); \
	st23 = ROTL64(st15 ^ bc[4], 41); \
	st15 = ROTL64(st4 ^ bc[3], 27); \
	st4 = ROTL64(st24 ^ bc[3], 14); \
	st24 = ROTL64(st21 ^ bc[0], 2); \
	st21 = ROTL64(st8 ^ bc[2], 55); \
	st8 = ROTL64(st16 ^ bc[0], 45); \
	st16 = ROTL64(st5 ^ bc[4], 36); \
	st5 = ROTL64(st3 ^ bc[2], 28); \
	st3 = ROTL64(st18 ^ bc[2], 21); \
	st18 = ROTL64(st17 ^ bc[1], 15); \
	st17 = ROTL64(st11 ^ bc[0], 10); \
	st11 = ROTL64(st7 ^ bc[1], 6); \
	st7 = ROTL64(st10 ^ bc[4], 3); \
	st10 = ROTL641(tmp1); \
	\
	tmp1 = st0; tmp2 = st1; st0 = bitselect(st0 ^ st2, st0, st1); st1 = bitselect(st1 ^ st3, st1, st2); st2 = bitselect(st2 ^ st4, st2, st3); st3 = bitselect(st3 ^ tmp1, st3, st4); st4 = bitselect(st4 ^ tmp2, st4, tmp1); \
	tmp1 = st5; tmp2 = st6; st5 = bitselect(st5 ^ st7, st5, st6); st6 = bitselect(st6 ^ st8, st6, st7); st7 = bitselect(st7 ^ st9, st7, st8); st8 = bitselect(st8 ^ tmp1, st8, st9); st9 = bitselect(st9 ^ tmp2, st9, tmp1); \
	tmp1 = st10; tmp2 = st11; st10 = bitselect(st10 ^ st12, st10, st11); st11 = bitselect(st11 ^ st13, st11, st12); st12 = bitselect(st12 ^ st14, st12, st13); st13 = bitselect(st13 ^ tmp1, st13, st14); st14 = bitselect(st14 ^ tmp2, st14, tmp1); \
	tmp1 = st15; tmp2 = st16; st15 = bitselect(st15 ^ st17, st15, st16); st16 = bitselect(st16 ^ st18, st16, st17); st17 = bitselect(st17 ^ st19, st17, st18); st18 = bitselect(st18 ^ tmp1, st18, st19); st19 = bitselect(st19 ^ tmp2, st19, tmp1); \
	tmp1 = st20; tmp2 = st21; st20 = bitselect(st20 ^ st22, st20, st21); st21 = bitselect(st21 ^ st23, st21, st22); st22 = bitselect(st22 ^ st24, st22, st23); st23 = bitselect(st23 ^ tmp1, st23, st24); st24 = bitselect(st24 ^ tmp2, st24, tmp1); \
	st0 ^= 1;

#define LASTRND1() \
	bc[0] = st0 ^ st5 ^ st10 * st15 * st20 ^ ROTL64(st2 ^ st7 ^ st12 * st17 * st22, 1); \
	bc[1] = st1 ^ st6 ^ st11 * st16 * st21 ^ ROTL64(st3 ^ st8 ^ st13 * st18 * st23, 1); \
	bc[2] = st2 ^ st7 ^ st12 * st17 * st22 ^ ROTL64(st4 ^ st9 ^ st14 * st19 * st24, 1); \
	bc[3] = st3 ^ st8 ^ st13 * st18 * st23 ^ ROTL64(st0 ^ st5 ^ st10 * st15 * st20, 1); \
	bc[4] = st4 ^ st9 ^ st14 * st19 * st24 ^ ROTL64(st1 ^ st6 ^ st11 * st16 * st21, 1); \
	\
	st0 ^= bc[4]; \
	st1 = ROTL64(st6 ^ bc[0], 44); \
	st2 = ROTL64(st12 ^ bc[1], 43); \
	st4 = ROTL64(st24 ^ bc[3], 14); \
	st3 = ROTL64(st18 ^ bc[2], 21); \
	\
	tmp1 = st0; st0 = bitselect(st0 ^ st2, st0, st1); st1 = bitselect(st1 ^ st3, st1, st2); st2 = bitselect(st2 ^ st4, st2, st3); st3 = bitselect(st3 ^ tmp1, st3, st4); \
	st0 ^= 1;

#define LASTRND2() \
	bc[2] = st2 ^ st7 ^ st12 * st17 * st22 ^ ROTL64(st4 ^ st9 ^ st14 * st19 * st24, 1); \
	bc[3] = st3 ^ st8 ^ st13 * st18 * st23 ^ ROTL64(st0 ^ st5 ^ st10 * st15 * st20, 1); \
	bc[4] = st4 ^ st9 ^ st14 * st19 * st24 ^ ROTL64(st1 ^ st6 ^ st11 * st16 * st21, 1); \
	\
	st0 ^= bc[4]; \
	st4 = ROTL64(st24 ^ bc[3], 14); \
	st3 = ROTL64(st18 ^ bc[2], 21); \
	st3 = bitselect(st3 ^ st0, st3, st4);

__device__ ulonglong4 operator^(const ulonglong4 &a, const ulonglong4 &b)
{
	return(make_ulonglong4(a.x ^ b.x, a.y ^ b.y, a.z ^ b.z, a.w ^ b.w));
}

#define MIX(vst) vst = vst ^ scratchpad[vst.x % scr_size] ^ scratchpad[vst.y % scr_size] ^ scratchpad[vst.z % scr_size] ^ scratchpad[vst.w % scr_size];

#define MIX_ALL MIX(vst0); MIX(vst4); MIX(vst8); MIX(vst12); MIX(vst16); MIX(vst20);

__global__
void wk(uint32_t * __restrict__ retnonce, const uint64_t * __restrict__ input, const ulonglong4 * __restrict__ scratchpad, const uint32_t scr_size, uint64_t nonce, const uint32_t target)
{
	ulonglong4 vst0, vst4, vst8, vst12, vst16, vst20;
	uint64_t __restrict__ bc[5], st24, tmp1, tmp2;

	nonce += (blockDim.x * blockIdx.x) + threadIdx.x;

	vst0 	= make_ulonglong4((nonce << 8) + (input[0] & 0xFF), input[1] & 0xFFFFFFFFFFFFFF00ULL, input[2], input[3]);
	vst4 	= make_ulonglong4(input[4], input[5], input[6], input[7]);
	vst8 	= make_ulonglong4(input[8], input[9], (input[10] & 0xFF) | 0x100, 0);
	vst12 	= make_ulonglong4(0, 0, 0, 0);
	vst16 	= make_ulonglong4(0x8000000000000000ULL, 0, 0, 0);
	vst20	= make_ulonglong4(0, 0, 0, 0);
	st24 	= 0;

	RND();
	MIX_ALL;

	for(int i = 0; i < 22; ++i)
	{
		RND();
		MIX_ALL;
	}

	LASTRND1();

	vst4 	= make_ulonglong4(1, 0, 0, 0);
	vst8 	= make_ulonglong4(0, 0, 0, 0);
	vst12 	= make_ulonglong4(0, 0, 0, 0);
	vst16	= make_ulonglong4(0x8000000000000000ULL, 0, 0, 0);
	vst20	= make_ulonglong4(0, 0, 0, 0);
	st24	= 0;

	RND();
	MIX_ALL;

	for(int i = 0; i < 22; ++i)
	{
		RND();
		MIX_ALL;
	}

	LASTRND2();

	if((st3 >> 32) <= target) *retnonce = (uint32_t)nonce;
}

extern "C" void UpdateScratchpad(uint32_t threads)
{
	for(int i = 0; i < threads; ++i)
		cudaMemcpyAsync(d_scratchpad[i], pscratchpad_buff, scratchpad_size << 3, cudaMemcpyHostToDevice, scr_copy_streams[i]);
}

extern "C" void InitCUDA(uint32_t threads, char **devstrs)
{
	struct cudaDeviceProp prop;
	int numdevs;

	if(cudaGetDeviceCount(&numdevs) != cudaSuccess)
	{
		applog(LOG_ERR, "Something's fucked - can't get number of CUDA devices.");
		exit(0);
	}

	if(threads > numdevs)
	{
		applog(LOG_ERR, "You specified more threads than there are CUDA devices, you idiot.");
		exit(0);
	}

	scr_copy_streams = (cudaStream_t *)malloc(sizeof(cudaStream_t) * threads);

	d_scratchpad = (ulonglong4 **)malloc(sizeof(ulonglong4 *) * threads);
	d_input = (uint64_t **)malloc(sizeof(uint64_t *) * threads);
	d_retnonce = (uint32_t **)malloc(sizeof(uint32_t *) * threads);

	for(int i = 0; i < threads; ++i)
	{
		cudaGetDeviceProperties(&prop, i);
		devstrs[i] = strdup(prop.name);
	}

}

extern "C" void CUDASetDevice(uint32_t thread_id)
{
	int i = (int) thread_id;
	cudaSetDevice(i);
	cudaDeviceReset();
	cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);

	#ifdef USE_MAPPED_MEMORY
	cudaSetDeviceFlags(cudaDeviceMapHost);
	cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
	#endif

	cudaMalloc(&d_scratchpad[i], WILD_KECCAK_SCRATCHPAD_BUFFSIZE);

#ifdef USE_MAPPED_MEMORY
	cudaHostAlloc(&d_retnonce[i], sizeof(uint32_t), cudaHostAllocMapped);
#else
	cudaMalloc(&d_retnonce[i], sizeof(uint32_t));
#endif
	cudaMalloc(&d_input[i], 88);
	cudaStreamCreate(&scr_copy_streams[i]);
}

extern "C" int scanhash_wildkeccak(int thr_id, uint32_t *pdata, const uint32_t *ptarget, uint32_t max_nonce, unsigned long *hashes_done)
{
	uint32_t *nonceptr = ((uint32_t *)(((uint8_t *)pdata) + 1));
	uint32_t n = *nonceptr;
	uint32_t first = n, blocks = CUDABlocks, threads = CUDAThreads;

	cudaMemcpy(d_input[thr_id], pdata, 88, cudaMemcpyHostToDevice);

#ifdef USE_MAPPED_MEMORY
	*(d_retnonce[thr_id]) = 0xFFFFFFFFUL;
	uint32_t *dnonce;
	cudaHostGetDevicePointer(&dnonce, d_retnonce[thr_id], 0);
#else
	cudaMemset(d_retnonce[thr_id], 0xFF, sizeof(uint32_t));
	uint32_t h_retnonce;
#endif

	cudaStreamSynchronize(scr_copy_streams[thr_id]);

	do
	{
		dim3 block(blocks);
		dim3 thread(threads);

#ifdef USE_MAPPED_MEMORY
		wk<<<block, thread, 0, scr_copy_streams[thr_id]>>>(dnonce, d_input[thr_id], d_scratchpad[thr_id], (uint32_t)(scratchpad_size >> 2), n, ptarget[7]);
		//cudaDeviceSynchronize();
		if(*(d_retnonce[thr_id]) < 0xFFFFFFFFU)
		{
			*nonceptr = *(retnonce[thr_id]);
			*hashes_done = *(retnonce[thr_id]) - first + 1;
			return(1);
		}
#else
		wk<<<block, thread, 0, scr_copy_streams[thr_id]>>>(d_retnonce[thr_id], d_input[thr_id], d_scratchpad[thr_id], (uint32_t)(scratchpad_size >> 2), n, ptarget[7]);
		//cudaDeviceSynchronize();
		cudaMemcpy(&h_retnonce, d_retnonce[thr_id], sizeof(uint32_t), cudaMemcpyDeviceToHost);
		if(h_retnonce < 0xFFFFFFFFU)
		{
			*nonceptr = h_retnonce;
			*hashes_done = h_retnonce - first + 1;
			return(1);
		}
#endif

		n += blocks * threads;
	} while(n < max_nonce && !work_restart[thr_id].restart);

	*hashes_done = n - first + 1;
	return(0);
}
