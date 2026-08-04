/*  fs_cuda_shim.h — minimal CUDA Driver API surface, loaded at runtime from
    nvcuda.dll (which ships with the NVIDIA DRIVER, not the toolkit).

    Adapted from EdgeRepair/src/gpu/ER_CudaShim.h, the suite's GPU reference
    (../../../docs/AE_GPU_Effects.md §1.2). Kept as a per-plugin copy rather
    than shared, matching the deliberate no-shared-plugin-code structure the
    suite README describes; the two differ only in the entry points each needs
    (this one adds cuMemGetInfo for the scratch budget and cuMemcpyHtoDAsync for
    the Gaussian weight upload).

    WHY A SHIM: the plugin must COMPILE and run on machines with no CUDA toolkit
    installed, and on non-NVIDIA machines, where nvcuda.dll does not exist,
    Get().loaded stays false, the CUDA backend declines, and AE falls back to the
    CPU reference. Only the .cu -> PTX build step needs the toolkit (nvcc); the
    host side needs exactly the handful of driver entry points declared here.
    Types/signatures mirror cuda.h for the calls we use — keep this file tiny;
    add entries only when a new driver call is actually needed.

    Context discipline (per PF_GPUDeviceSuite1's header note): push AE's
    CUcontext around every driver call sequence and pop after — FS_CudaCtxGuard
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

#define FS_CUDA_SUCCESS 0

/* All driver API entry points are __stdcall on Win32 (CUDAAPI). */
typedef CUresult (__stdcall *FS_PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult (__stdcall *FS_PFN_cuCtxPopCurrent)(CUcontext*);
typedef CUresult (__stdcall *FS_PFN_cuModuleLoadDataEx)(
    CUmodule*, const void*, unsigned int, int*, void**);
typedef CUresult (__stdcall *FS_PFN_cuModuleUnload)(CUmodule);
typedef CUresult (__stdcall *FS_PFN_cuModuleGetFunction)(
    CUfunction*, CUmodule, const char*);
typedef CUresult (__stdcall *FS_PFN_cuLaunchKernel)(
    CUfunction,
    unsigned int, unsigned int, unsigned int,   /* grid  x y z */
    unsigned int, unsigned int, unsigned int,   /* block x y z */
    unsigned int,                               /* shared mem bytes */
    CUstream,
    void**,                                     /* kernel params */
    void**);                                    /* extra (NULL) */
typedef CUresult (__stdcall *FS_PFN_cuMemcpyHtoDAsync)(
    CUdeviceptr, const void*, size_t, CUstream);
typedef CUresult (__stdcall *FS_PFN_cuMemGetInfo)(size_t*, size_t*);
typedef CUresult (__stdcall *FS_PFN_cuMemsetD8Async)(
    CUdeviceptr, unsigned char, size_t, CUstream);

/* Loaded once per process; immutable after initialization (write-once, then
   read-only — MFR-safe by the same argument as a const table; the C++11
   magic-static guarantees thread-safe init). */
struct FS_CudaApi {
    bool                        loaded = false;
    FS_PFN_cuCtxPushCurrent     CtxPushCurrent    = nullptr;
    FS_PFN_cuCtxPopCurrent      CtxPopCurrent     = nullptr;
    FS_PFN_cuModuleLoadDataEx   ModuleLoadDataEx  = nullptr;
    FS_PFN_cuModuleUnload       ModuleUnload      = nullptr;
    FS_PFN_cuModuleGetFunction  ModuleGetFunction = nullptr;
    FS_PFN_cuLaunchKernel       LaunchKernel      = nullptr;
    FS_PFN_cuMemcpyHtoDAsync    MemcpyHtoDAsync   = nullptr;
    /* Diagnostics / soft-optional, deliberately NOT part of the `loaded` gate:
       a missing symbol must not disable CUDA. MemGetInfo absent -> the budget
       falls back to a fixed default; MemsetD8Async absent -> the visible marker
       simply does not draw. */
    FS_PFN_cuMemGetInfo         MemGetInfo        = nullptr;
    FS_PFN_cuMemsetD8Async      MemsetD8Async     = nullptr;

    static const FS_CudaApi& Get()
    {
        static const FS_CudaApi api = Load();
        return api;
    }

private:
    static FS_CudaApi Load()
    {
        FS_CudaApi a;
        /* Never freed: driver-API state must outlive every CUDA object we
           create, and the module is process-lifetime anyway. */
        HMODULE h = ::LoadLibraryA("nvcuda.dll");
        if (!h) return a;   /* non-NVIDIA machine — CUDA backend declines */

        a.CtxPushCurrent    = reinterpret_cast<FS_PFN_cuCtxPushCurrent>(
                                  ::GetProcAddress(h, "cuCtxPushCurrent_v2"));
        a.CtxPopCurrent     = reinterpret_cast<FS_PFN_cuCtxPopCurrent>(
                                  ::GetProcAddress(h, "cuCtxPopCurrent_v2"));
        a.ModuleLoadDataEx  = reinterpret_cast<FS_PFN_cuModuleLoadDataEx>(
                                  ::GetProcAddress(h, "cuModuleLoadDataEx"));
        a.ModuleUnload      = reinterpret_cast<FS_PFN_cuModuleUnload>(
                                  ::GetProcAddress(h, "cuModuleUnload"));
        a.ModuleGetFunction = reinterpret_cast<FS_PFN_cuModuleGetFunction>(
                                  ::GetProcAddress(h, "cuModuleGetFunction"));
        a.LaunchKernel      = reinterpret_cast<FS_PFN_cuLaunchKernel>(
                                  ::GetProcAddress(h, "cuLaunchKernel"));
        a.MemcpyHtoDAsync   = reinterpret_cast<FS_PFN_cuMemcpyHtoDAsync>(
                                  ::GetProcAddress(h, "cuMemcpyHtoDAsync_v2"));
        a.MemGetInfo        = reinterpret_cast<FS_PFN_cuMemGetInfo>(
                                  ::GetProcAddress(h, "cuMemGetInfo_v2"));
        a.MemsetD8Async     = reinterpret_cast<FS_PFN_cuMemsetD8Async>(
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
struct FS_CudaCtxGuard {
    const FS_CudaApi &api;
    bool pushed;
    explicit FS_CudaCtxGuard(const FS_CudaApi &a, CUcontext ctx)
        : api(a), pushed(false)
    {
        if (api.loaded && ctx)
            pushed = (api.CtxPushCurrent(ctx) == FS_CUDA_SUCCESS);
    }
    ~FS_CudaCtxGuard()
    {
        if (pushed) {
            CUcontext dummy = nullptr;
            api.CtxPopCurrent(&dummy);
        }
    }
    FS_CudaCtxGuard(const FS_CudaCtxGuard&) = delete;
    FS_CudaCtxGuard& operator=(const FS_CudaCtxGuard&) = delete;
};

#endif /* _WIN32 */
