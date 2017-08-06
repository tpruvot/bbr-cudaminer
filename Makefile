CUDA	= /usr/local/cuda

CC	= gcc
NVCC	= $(CUDA)/bin/nvcc

CFLAGS	= -std=gnu11 -Ofast -c
LD_LIBS	= -lcurl -ljansson

OPTS	= #-DUSE_MAPPED_MEMORY
NVFLAGS	= $(OPTS) -O3 -Xptxas "-v" --restrict --use_fast_math

SM_ARCH  = -gencode=arch=compute_50,code=\"sm_50,compute_50\"
SM_ARCH += -gencode=arch=compute_52,code=\"sm_52,compute_52\"

ifeq ($(CUDA),/usr/local/cuda-8.0)
    SM_ARCH := -gencode=arch=compute_61,code=\"sm_61,compute_61\" $(SM_ARCH)
endif

all:
	$(CC) $(CFLAGS) cpu-miner.c -o cpu-miner.o
	$(CC) $(CFLAGS) util.c -o util.o
	$(CC) $(CFLAGS) wildkeccak.c -o wildkeccak.o
	$(NVCC) $(NVFLAGS) $(SM_ARCH) cpu-miner.o util.o wildkeccak.o wildkeccak.cu $(LD_LIBS) -o cudaminerd

clean:
	rm -rf *.o cudaminerd
