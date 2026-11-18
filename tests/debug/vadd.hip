// A program for the GPU debugging tests: a kernel that adds two vectors,
// launched as one block of 64 threads.
#include <hip/hip_runtime.h>
#include <cstdio>

__global__ void vadd(const int* a, const int* b, int* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        int sum = a[i] + b[i];
        c[i] = sum;
    }
}

int main() {
    const int n = 64;
    int ha[n], hb[n], hc[n];
    for (int i = 0; i < n; ++i) ha[i] = i, hb[i] = 2 * i;
    int *da, *db, *dc;
    hipMalloc(&da, sizeof ha);
    hipMalloc(&db, sizeof hb);
    hipMalloc(&dc, sizeof hc);
    hipMemcpy(da, ha, sizeof ha, hipMemcpyHostToDevice);
    hipMemcpy(db, hb, sizeof hb, hipMemcpyHostToDevice);
    vadd<<<1, n>>>(da, db, dc, n);
    hipMemcpy(hc, dc, sizeof hc, hipMemcpyDeviceToHost);
    std::printf("c[10]=%d\n", hc[10]);
    return 0;
}
