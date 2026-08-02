/*  ER_CudaShim.h — minimal CUDA Driver API surface, loaded at runtime from
    nvcuda.dll (which ships with the NVIDIA DRIVER, not the toolkit).

    WHY A SHIM: the plugin must COMPILE and run on machines with no CUDA
    toolkit installed (and on non-NVIDIA machines, where nvcuda.dll simply
    doesn't exist and Get().loaded stays false → the CUDA backend declines and
    AE falls back to CPU). Only the .cu → PTX build step needs the toolkit
    (nvcc); the host side needs exactly the handful of driver entry points
    declared here. Types/signatures mirror cuda.h for the calls we use —
    keep this file tiny; add entries only when a new driver call is needed.

    Context discipline (per PF_GPUDeviceSuite1's header note): push AE's
    CUcontext around every driver call sequence and pop after —
    ER_CudaCtxGuard below.
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

#define ER_CUDA_SUCCESS 0

/* All driver API entry points are __stdcall on Win32 (CUDAAPI). */
typedef CUresult (__stdcall *ER_PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult (__stdcall *ER_PFN_cuCtxPopCurrent)(CUcontext*);
typedef CUresult (__stdcall *ER_PFN_cuModuleLoadDataEx)(
    CUmodule*, const void*, unsigned int, int*, void**);
typedef CUresult (__stdcall *ER_PFN_cuModuleUnload)(CUmodule);
typedef CUresult (__stdcall *ER_PFN_cuModuleGetFunction)(
    CUfunction*, CUmodule, const char*);
typedef CUresult (__stdcall *ER_PFN_cuLaunchKernel)(
    CUfunction,
    unsigned int, unsigned int, unsigned int,   /* grid  x y z */
    unsigned int, unsigned int, unsigned int,   /* block x y z */
    unsigned int,                               /* shared mem bytes */
    CUstream,
    void**,                                     /* kernel params */
    void**);                                    /* extra (NULL) */
typedef CUresult (__stdcall *ER_PFN_cuMemcpyDtoDAsync)(
    CUdeviceptr, CUdeviceptr, size_t, CUstream);
typedef CUresult (__stdcall *ER_PFN_cuMemsetD8Async)(
    CUdeviceptr, unsigned char, size_t, CUstream);

/* Loaded once per process; immutable after initialization (write-once, then
   read-only — MFR-safe by the same argument as a const table; the C++11
   magic-static guarantees thread-safe init). */
struct ER_CudaApi {
    bool                        loaded = false;
    ER_PFN_cuCtxPushCurrent     CtxPushCurrent   = nullptr;
    ER_PFN_cuCtxPopCurrent      CtxPopCurrent    = nullptr;
    ER_PFN_cuModuleLoadDataEx   ModuleLoadDataEx = nullptr;
    ER_PFN_cuModuleUnload       ModuleUnload     = nullptr;
    ER_PFN_cuModuleGetFunction  ModuleGetFunction= nullptr;
    ER_PFN_cuLaunchKernel       LaunchKernel     = nullptr;
    ER_PFN_cuMemcpyDtoDAsync    MemcpyDtoDAsync  = nullptr;
    /* Diagnostic only (visible-marker passthrough); deliberately NOT part of
       the `loaded` gate below — a missing symbol must not disable CUDA. */
    ER_PFN_cuMemsetD8Async      MemsetD8Async    = nullptr;

    static const ER_CudaApi& Get()
    {
        static const ER_CudaApi api = Load();
        return api;
    }

private:
    static ER_CudaApi Load()
    {
        ER_CudaApi a;
        /* Never freed: driver-API state must outlive every CUDA object we
           create, and the module is process-lifetime anyway. */
        HMODULE h = ::LoadLibraryA("nvcuda.dll");
        if (!h) return a;   /* non-NVIDIA machine — CUDA backend declines */

        a.CtxPushCurrent    = reinterpret_cast<ER_PFN_cuCtxPushCurrent>(
                                  ::GetProcAddress(h, "cuCtxPushCurrent_v2"));
        a.CtxPopCurrent     = reinterpret_cast<ER_PFN_cuCtxPopCurrent>(
                                  ::GetProcAddress(h, "cuCtxPopCurrent_v2"));
        a.ModuleLoadDataEx  = reinterpret_cast<ER_PFN_cuModuleLoadDataEx>(
                                  ::GetProcAddress(h, "cuModuleLoadDataEx"));
        a.ModuleUnload      = reinterpret_cast<ER_PFN_cuModuleUnload>(
                                  ::GetProcAddress(h, "cuModuleUnload"));
        a.ModuleGetFunction = reinterpret_cast<ER_PFN_cuModuleGetFunction>(
                                  ::GetProcAddress(h, "cuModuleGetFunction"));
        a.LaunchKernel      = reinterpret_cast<ER_PFN_cuLaunchKernel>(
                                  ::GetProcAddress(h, "cuLaunchKernel"));
        a.MemcpyDtoDAsync   = reinterpret_cast<ER_PFN_cuMemcpyDtoDAsync>(
                                  ::GetProcAddress(h, "cuMemcpyDtoDAsync_v2"));
        a.MemsetD8Async     = reinterpret_cast<ER_PFN_cuMemsetD8Async>(
                                  ::GetProcAddress(h, "cuMemsetD8Async"));

        a.loaded = a.CtxPushCurrent && a.CtxPopCurrent &&
                   a.ModuleLoadDataEx && a.ModuleUnload &&
                   a.ModuleGetFunction && a.LaunchKernel &&
                   a.MemcpyDtoDAsync;
        return a;
    }
};

/* Push AE's context for a scope of driver calls, pop on exit (the suite
   header's documented discipline for CUDA under AE). */
struct ER_CudaCtxGuard {
    const ER_CudaApi &api;
    bool pushed;
    explicit ER_CudaCtxGuard(const ER_CudaApi &a, CUcontext ctx)
        : api(a), pushed(false)
    {
        if (api.loaded && ctx)
            pushed = (api.CtxPushCurrent(ctx) == ER_CUDA_SUCCESS);
    }
    ~ER_CudaCtxGuard()
    {
        if (pushed) {
            CUcontext dummy = nullptr;
            api.CtxPopCurrent(&dummy);
        }
    }
};

#endif /* _WIN32 */
