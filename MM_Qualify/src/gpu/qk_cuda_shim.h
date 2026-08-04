/*  qk_cuda_shim.h — minimal CUDA Driver API surface, loaded at runtime from
    nvcuda.dll (which ships with the NVIDIA DRIVER, not the toolkit).

    Adapted from MM_FreqSep/src/gpu/fs_cuda_shim.h, itself adapted from
    EdgeRepair/src/gpu/ER_CudaShim.h — the suite's GPU reference
    (../../../docs/AE_GPU_Effects.md §1.2). Kept as a per-plugin copy rather
    than shared, matching the deliberate no-shared-plugin-code structure the
    suite README describes.

    WHY A SHIM: the plugin must COMPILE and run on machines with no CUDA
    toolkit installed, and on non-NVIDIA machines, where nvcuda.dll does not
    exist, Get().loaded stays false, the CUDA backend declines, and AE falls
    back to the CPU reference. Only the .cu -> PTX build step needs nvcc; the
    host side needs exactly the handful of driver entry points declared here.
    Types/signatures mirror cuda.h for the calls we use — keep this file tiny;
    add entries only when a new driver call is actually needed.

    Context discipline (per PF_GPUDeviceSuite1's header note): push AE's
    CUcontext around every driver call sequence and pop after — QK_CudaCtxGuard
    below. That includes calls we only TRIGGER, such as the AE suite's own
    device allocator.
*/

#pragma once

#ifdef _WIN32

#include <Windows.h>

/* ---- Types (ABI-compatible with cuda.h for the entry points below) ---- */
typedef int                     CUresult;      /* CUDA_SUCCESS == 0 */
typedef unsigned long long      CUdeviceptr;   /* device pointer, 64-bit */
typedef struct CUctx_st        *CUcontext;
typedef struct CUmod_st        *CUmodule;
typedef struct CUfunc_st       *CUfunction;
typedef struct CUstream_st     *CUstream;

#define QK_CUDA_SUCCESS 0

/* All driver API entry points are __stdcall on Win32 (CUDAAPI). */
typedef CUresult (__stdcall *QK_PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult (__stdcall *QK_PFN_cuCtxPopCurrent)(CUcontext*);
typedef CUresult (__stdcall *QK_PFN_cuModuleLoadDataEx)(
    CUmodule*, const void*, unsigned int, int*, void**);
typedef CUresult (__stdcall *QK_PFN_cuModuleUnload)(CUmodule);
typedef CUresult (__stdcall *QK_PFN_cuModuleGetFunction)(
    CUfunction*, CUmodule, const char*);
typedef CUresult (__stdcall *QK_PFN_cuLaunchKernel)(
    CUfunction,
    unsigned int, unsigned int, unsigned int,   /* grid  x y z */
    unsigned int, unsigned int, unsigned int,   /* block x y z */
    unsigned int,                               /* shared mem bytes */
    CUstream,
    void**,                                     /* kernel params */
    void**);                                    /* extra (NULL) */
typedef CUresult (__stdcall *QK_PFN_cuMemcpyHtoDAsync)(
    CUdeviceptr, const void*, size_t, CUstream);
typedef CUresult (__stdcall *QK_PFN_cuMemGetInfo)(size_t*, size_t*);
typedef CUresult (__stdcall *QK_PFN_cuMemsetD8Async)(
    CUdeviceptr, unsigned char, size_t, CUstream);

/* Loaded once per process; immutable after initialization (write-once, then
   read-only — MFR-safe by the same argument as a const table; the C++11
   magic-static guarantees thread-safe init). */
struct QK_CudaApi {
    bool                        loaded = false;
    QK_PFN_cuCtxPushCurrent     CtxPushCurrent    = nullptr;
    QK_PFN_cuCtxPopCurrent      CtxPopCurrent     = nullptr;
    QK_PFN_cuModuleLoadDataEx   ModuleLoadDataEx  = nullptr;
    QK_PFN_cuModuleUnload       ModuleUnload      = nullptr;
    QK_PFN_cuModuleGetFunction  ModuleGetFunction = nullptr;
    QK_PFN_cuLaunchKernel       LaunchKernel      = nullptr;
    QK_PFN_cuMemcpyHtoDAsync    MemcpyHtoDAsync   = nullptr;
    /* Diagnostics / soft-optional, deliberately NOT part of the `loaded` gate:
       a missing symbol must not disable CUDA. MemGetInfo absent -> the budget
       falls back to a fixed default; MemsetD8Async absent -> the visible
       marker simply does not draw. */
    QK_PFN_cuMemGetInfo         MemGetInfo        = nullptr;
    QK_PFN_cuMemsetD8Async      MemsetD8Async     = nullptr;

    static const QK_CudaApi& Get()
    {
        static const QK_CudaApi api = Load();
        return api;
    }

private:
    static QK_CudaApi Load()
    {
        QK_CudaApi a;
        /* Never freed: driver-API state must outlive every CUDA object we
           create, and the module is process-lifetime anyway. */
        HMODULE h = ::LoadLibraryA("nvcuda.dll");
        if (!h) return a;   /* non-NVIDIA machine — CUDA backend declines */

        a.CtxPushCurrent    = reinterpret_cast<QK_PFN_cuCtxPushCurrent>(
                                  ::GetProcAddress(h, "cuCtxPushCurrent_v2"));
        a.CtxPopCurrent     = reinterpret_cast<QK_PFN_cuCtxPopCurrent>(
                                  ::GetProcAddress(h, "cuCtxPopCurrent_v2"));
        a.ModuleLoadDataEx  = reinterpret_cast<QK_PFN_cuModuleLoadDataEx>(
                                  ::GetProcAddress(h, "cuModuleLoadDataEx"));
        a.ModuleUnload      = reinterpret_cast<QK_PFN_cuModuleUnload>(
                                  ::GetProcAddress(h, "cuModuleUnload"));
        a.ModuleGetFunction = reinterpret_cast<QK_PFN_cuModuleGetFunction>(
                                  ::GetProcAddress(h, "cuModuleGetFunction"));
        a.LaunchKernel      = reinterpret_cast<QK_PFN_cuLaunchKernel>(
                                  ::GetProcAddress(h, "cuLaunchKernel"));
        a.MemcpyHtoDAsync   = reinterpret_cast<QK_PFN_cuMemcpyHtoDAsync>(
                                  ::GetProcAddress(h, "cuMemcpyHtoDAsync_v2"));
        a.MemGetInfo        = reinterpret_cast<QK_PFN_cuMemGetInfo>(
                                  ::GetProcAddress(h, "cuMemGetInfo_v2"));
        a.MemsetD8Async     = reinterpret_cast<QK_PFN_cuMemsetD8Async>(
                                  ::GetProcAddress(h, "cuMemsetD8Async"));

        a.loaded = a.CtxPushCurrent && a.CtxPopCurrent &&
                   a.ModuleLoadDataEx && a.ModuleUnload &&
                   a.ModuleGetFunction && a.LaunchKernel &&
                   a.MemcpyHtoDAsync;
        return a;
    }
};

/* Push AE's context for a scope of driver calls, pop on exit (the suite
   header's documented discipline for CUDA under AE). */
struct QK_CudaCtxGuard {
    const QK_CudaApi &api;
    bool pushed;
    explicit QK_CudaCtxGuard(const QK_CudaApi &a, CUcontext ctx)
        : api(a), pushed(false)
    {
        if (api.loaded && ctx)
            pushed = (api.CtxPushCurrent(ctx) == QK_CUDA_SUCCESS);
    }
    ~QK_CudaCtxGuard()
    {
        if (pushed) {
            CUcontext dummy = nullptr;
            api.CtxPopCurrent(&dummy);
        }
    }
    QK_CudaCtxGuard(const QK_CudaCtxGuard&) = delete;
    QK_CudaCtxGuard& operator=(const QK_CudaCtxGuard&) = delete;
};

#endif /* _WIN32 */
