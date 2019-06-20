#include "spmv.h"
#include <cuda_runtime.h>
#include "cusparse.h"
#define FULL_MASK 0xffffffff

template <int K, typename T>
__global__
void csr_v_k_shuffle(int n, int *d_ia, int *d_ja, T *d_a, T *d_x, T *d_y)
{
   /*------------------------------------------------------------*
    *               CSR spmv-vector kernel
    *              shuffle version reduction
    *            K-Warp  ( K threads) per row
    *------------------------------------------------------------*/
   // num of full-warps
   int nw = gridDim.x*BLOCKDIM/K;
   // full warp id
   int wid = (blockIdx.x*BLOCKDIM+threadIdx.x)/K;
   // thread lane in each full warp
   int lane = threadIdx.x & (K-1);
   // shared memory for patial result
#ifdef ROW_PTR_USE_SHARED
   // full warp lane in each block
   int wlane = threadIdx.x/K;
   volatile __shared__ int startend[BLOCKDIM/K][2];
#endif
   for (int row = wid; row < n; row += nw)
   {
#ifdef ROW_PTR_USE_SHARED
      // row start and end point
      if (lane < 2)
      {
         startend[wlane][lane] = d_ia[row+lane];
      }
      int p = startend[wlane][0];
      int q = startend[wlane][1];
#else
      int j, p, q;
      if (lane < 2)
      {
         j = __ldg(&d_ia[row+lane]);
      }
      p = __shfl_sync(0xFFFFFFFF, j, 0, K);
      q = __shfl_sync(0xFFFFFFFF, j, 1, K);
#endif
      T sum = 0.0;
      for (int i=p+lane; i<q; i+=K)
      {
         sum += d_a[i] * d_x[d_ja[i]];
      }

      // parallel reduction
#pragma unroll
      for (int d = K/2; d > 0; d >>= 1)
      {
         sum += __shfl_down_sync(FULL_MASK, sum, d, K);
      }
      if (lane == 0)
      {
         d_y[row] = sum;
      }
   }
}



void spmv_csr_vector(struct csr_t *csr, REAL *x, REAL *y)
{
   int *d_ia, *d_ja, i;
   REAL *d_a, *d_x, *d_y;
   int n = csr->nrows;
   int nnz = csr->ia[n];
   double t1,t2;
   /*---------- Device Memory */
   cudaMalloc((void **)&d_ia, (n+1)*sizeof(int));
   cudaMalloc((void **)&d_ja, nnz*sizeof(int));
   cudaMalloc((void **)&d_a, nnz*sizeof(REAL));
   cudaMalloc((void **)&d_x, n*sizeof(REAL));
   cudaMalloc((void **)&d_y, n*sizeof(REAL));
   /*---------- Memcpy */
   cudaMemcpy(d_ia, csr->ia, (n+1)*sizeof(int),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_ja, csr->ja, nnz*sizeof(int),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_a, csr->a, nnz*sizeof(REAL),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_x, x, n*sizeof(REAL),
   cudaMemcpyHostToDevice);
   /*-------- set spmv kernel */
   /*-------- num of half-warps per block */
   int hwb = BLOCKDIM/HALFWARP;
   int gDim = min(MAXTHREADS/BLOCKDIM, (n+hwb-1)/hwb);
   int bDim = BLOCKDIM;
   //printf("CSR<<<%4d, %3d>>>  ",gDim,bDim);
   if(nnz/n>16)
   {
      /*-------- start timing */
      t1 = wall_timer();
      for (i=0; i<REPEAT; i++)
      {
         //cudaMemset((void *)d_y, 0, n*sizeof(REAL));
         csr_v_k_shuffle<16, REAL> <<<gDim, bDim>>>(n, d_ia, d_ja, d_a, d_x, d_y);
      }
      /*-------- Barrier for GPU calls */
      cudaThreadSynchronize();
      /*-------- stop timing */
      t2 = wall_timer()-t1;   
   }
   else if(nnz/n>8)
   {
      /*-------- start timing */
      t1 = wall_timer();
      for (i=0; i<REPEAT; i++)
      {
         //cudaMemset((void *)d_y, 0, n*sizeof(REAL));
         csr_v_k_shuffle<8, REAL> <<<gDim, bDim>>>(n, d_ia, d_ja, d_a, d_x, d_y);
      }
      /*-------- Barrier for GPU calls */
      cudaThreadSynchronize();
      /*-------- stop timing */
      t2 = wall_timer()-t1;
   }
   else
   {
      /*-------- start timing */
      t1 = wall_timer();
      for (i=0; i<REPEAT; i++)
      {
         //cudaMemset((void *)d_y, 0, n*sizeof(REAL));
         csr_v_k_shuffle<4, REAL> <<<gDim, bDim>>>(n, d_ia, d_ja, d_a, d_x, d_y);
      }
      /*-------- Barrier for GPU calls */
      cudaThreadSynchronize();
      /*-------- stop timing */
      t2 = wall_timer()-t1;
   }
/*--------------------------------------------------*/
   printf("\n=== [GPU] CSR-vector Kernel ===\n");
   printf("  Number of Threads <%d*%d>\n",gDim,bDim);
   printf("  %.2f ms, %.2f GFLOPS, ",
   t2*1e3,2*nnz/t2/1e9*REPEAT);
/*-------- copy y to host mem */
   cudaMemcpy(y, d_y, n*sizeof(REAL),
   cudaMemcpyDeviceToHost);
/*---------- CUDA free */
   cudaFree(d_ia);
   cudaFree(d_ja);
   cudaFree(d_a);
   cudaFree(d_x);
   cudaFree(d_y);
}

/*-----------------------------------------------*/
void cuda_init(int argc, char **argv)
{
   int deviceCount, dev;
   cudaGetDeviceCount(&deviceCount);
   printf("=========================================\n");
   if (deviceCount == 0)
   {
      printf("There is no device supporting CUDA\n");
   }
   for (dev = 0; dev < deviceCount; ++dev)
   {
      cudaDeviceProp deviceProp;
      cudaGetDeviceProperties(&deviceProp, dev);
      if (dev == 0)
      {
         if (deviceProp.major == 9999 && deviceProp.minor == 9999)
         {
            printf("There is no device supporting CUDA.\n");
         }
         else if (deviceCount == 1)
         {
            printf("There is 1 device supporting CUDA\n");
         }
         else
         {
            printf("There are %d devices supporting CUDA\n", deviceCount);
         }
      }
   printf("\nDevice %d: \"%s\"\n", dev, deviceProp.name);
   printf("  Major revision number:          %d\n",deviceProp.major);
   printf("  Minor revision number:          %d\n",deviceProp.minor);
   printf("  Total amount of global memory:  %.2f GB\n",deviceProp.totalGlobalMem/1e9);
   }
   dev = 0;
   cudaSetDevice(dev);
   cudaDeviceProp deviceProp;
   cudaGetDeviceProperties(&deviceProp, dev);
   printf("\nRunning on Device %d: \"%s\"\n", dev, deviceProp.name);
   printf("=========================================\n");
}

/*---------------------------------------------------*/
void cuda_check_err()
{
   cudaError_t cudaerr = cudaGetLastError() ;
   if (cudaerr != cudaSuccess)
   {
       printf("error: %s\n",cudaGetErrorString(cudaerr));
   }
}

void spmv_cusparse_csr(struct csr_t *csr, REAL *x, REAL *y)
{
   int n = csr->nrows;
   int nnz = csr->ia[n];
   int *d_ia, *d_ja, i;
   REAL *d_a, *d_x, *d_y;
   double t1, t2;
   REAL done = 1.0, dzero = 0.0;
   /*------------------- allocate Device Memory */
   cudaMalloc((void **)&d_ia, (n+1)*sizeof(int));
   cudaMalloc((void **)&d_ja, nnz*sizeof(int));
   cudaMalloc((void **)&d_a, nnz*sizeof(REAL));
   cudaMalloc((void **)&d_x, n*sizeof(REAL));
   cudaMalloc((void **)&d_y, n*sizeof(REAL));
   /*------------------- Memcpy */
   cudaMemcpy(d_ia, csr->ia, (n+1)*sizeof(int),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_ja, csr->ja, nnz*sizeof(int),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_a, csr->a, nnz*sizeof(REAL),
   cudaMemcpyHostToDevice);
   cudaMemcpy(d_x, x, n*sizeof(REAL),
   cudaMemcpyHostToDevice);
   /*-------------------- cusparse library*/
   cusparseStatus_t status;
   cusparseHandle_t handle=0;
   cusparseMatDescr_t descr=0;

   /* initialize cusparse library */
   status= cusparseCreate(&handle);
   if (status != CUSPARSE_STATUS_SUCCESS)
   {
     printf("CUSPARSE Library initialization failed\n");
     exit(1);
   }
   /* create and setup matrix descriptor */
   status= cusparseCreateMatDescr(&descr);
   if (status != CUSPARSE_STATUS_SUCCESS)
   {
     printf("Matrix descriptor initialization failed\n");
     exit(1);
   }
   cusparseSetMatType(descr,CUSPARSE_MATRIX_TYPE_GENERAL);
   cusparseSetMatIndexBase(descr,CUSPARSE_INDEX_BASE_ZERO);
   /*-------- start timing */
   t1 = wall_timer();
   for (i=0; i<REPEAT; i++)
   {
#if DOUBLEPRECISION
      status= cusparseDcsrmv(handle, CUSPARSE_OPERATION_NON_TRANSPOSE, n, n, nnz,
      &done, descr, d_a, d_ia, d_ja,
      d_x, &dzero, d_y);
#else
      status= cusparseScsrmv(handle, CUSPARSE_OPERATION_NON_TRANSPOSE, n, n, nnz,
      &done, descr, d_a, d_ia, d_ja,
      d_x, &dzero, d_y);
#endif
      if (status != CUSPARSE_STATUS_SUCCESS)
      {
         printf("Matrix-vector multiplication failed\n");
         exit(1);
      }
   }
   /*-------- barrier for GPU calls */
   cudaThreadSynchronize();
   /*-------- stop timing */
   t2 = wall_timer()-t1;
   /*--------------------------------------------------*/
   printf("\n=== [GPU] CUSPARSE CSR Kernel ===\n");
   printf("  %.2f ms, %.2f GFLOPS, ",
   t2*1e3,2*nnz/t2/1e9*REPEAT);
   /*-------- copy y to host mem */
   cudaMemcpy(y, d_y, n*sizeof(REAL),
   cudaMemcpyDeviceToHost);
   /*--------- CUDA free */
   cudaFree(d_ia);
   cudaFree(d_ja);
   cudaFree(d_a);
   cudaFree(d_x);
   cudaFree(d_y);
   /* destroy matrix descriptor */
   status = cusparseDestroyMatDescr(descr);
   descr = 0;
   if (status != CUSPARSE_STATUS_SUCCESS)
   {
      printf("Matrix descriptor destruction failed\n");
      exit(1);
   }
   /* destroy handle */
   status = cusparseDestroy(handle);
   handle = 0;
   if (status != CUSPARSE_STATUS_SUCCESS)
   {
      printf("CUSPARSE Library release of resources failed\n");
      exit(1);
   }
}

