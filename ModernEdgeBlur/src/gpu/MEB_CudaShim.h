/*  MEB_CudaShim.h — minimal CUDA Driver API surface, loaded at runtime from
    nvcuda.dll (which ships with the NVIDIA DRIVER, not the toolkit).

    WHY A SHIM: the plugin must COMPILE and run on machines with no CUDA
    toolkit installed (and on non-NVIDIA machines, where nvcuda.dll simply
    doesn't exist and Get().loaded stays false -> the CUDA backend declines and
    AE falls back to CPU). Only the .cu -> PTX build step needs the toolkit
    (nvcc); the host side needs exactly the handful of driver entry points
    declared here. Types/signatures mirror cuda.h for the calls we use — keep
    this file tiny; add entries only when a new driver call is needed.

    Per-plugin copy by design, like every other shared-looking header in this
    suite: EdgeRepair owns src/gpu/ER_CudaShim.h, Modern Edge Blur owns this
    one. They are near-identical today and are allowed to diverge — this one
    additionally needs MemcpyHtoDAsync, because Modern Edge Blur uploads its
    Gaussian weights (built on the host in the CPU path's own MakeBlurKernel /
    MakeBlendKernel, so both paths convolve with the same bits — see
    gpu-kernel-rules.md §3.1).

    Context discipline (per PF_GPUDeviceSuite1's header note): push AE's
    CUcontext around every driver call sequence and pop after — MEB_CudaCtxGuard
    below.
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

#define MEB_CUDA_SUCCESS 0

/* All driver API entry points are __stdcall on Win32 (CUDAAPI). */
typedef CUresult (__stdcall *MEB_PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult (__stdcall *MEB_PFN_cuCtxPopCurrent)(CUcontext*);
typedef CUresult (__stdcall *MEB_PFN_cuModuleLoadDataEx)(
    CUmodule*, const void*, unsigned int, int*, void**);
typedef CUresult (__stdcall *MEB_PFN_cuModuleUnload)(CUmodule);
typedef CUresult (__stdcall *MEB_PFN_cuModuleGetFunction)(
    CUfunction*, CUmodule, const char*);
typedef CUresult (__stdcall *MEB_PFN_cuLaunchKernel)(
    CUfunction,
    unsigned int, unsigned int, unsigned int,   /* grid  x y z */
    unsigned int, unsigned int, unsigned int,   /* block x y z */
    unsigned int,                               /* shared mem bytes */
    CUstream,
    void**,                                     /* kernel params */
    void**);                                    /* extra (NULL) */
typedef CUresult (__stdcall *MEB_PFN_cuMemcpyHtoDAsync)(
    CUdeviceptr, const void*, size_t, CUstream);

/* Loaded once per process; immutable after initialization (write-once, then
   read-only — MFR-safe by the same argument as a const table; the C++11
   magic-static guarantees thread-safe init). */
struct MEB_CudaApi {
    bool                         loaded = false;
    MEB_PFN_cuCtxPushCurrent     CtxPushCurrent    = nullptr;
    MEB_PFN_cuCtxPopCurrent      CtxPopCurrent     = nullptr;
    MEB_PFN_cuModuleLoadDataEx   ModuleLoadDataEx  = nullptr;
    MEB_PFN_cuModuleUnload       ModuleUnload      = nullptr;
    MEB_PFN_cuModuleGetFunction  ModuleGetFunction = nullptr;
    MEB_PFN_cuLaunchKernel       LaunchKernel      = nullptr;
    MEB_PFN_cuMemcpyHtoDAsync    MemcpyHtoDAsync   = nullptr;

    static const MEB_CudaApi& Get()
    {
        static const MEB_CudaApi api = Load();
        return api;
    }

private:
    static MEB_CudaApi Load()
    {
        MEB_CudaApi a;
        /* Never freed: driver-API state must outlive every CUDA object we
           create, and the module is process-lifetime anyway. */
        HMODULE h = ::LoadLibraryA("nvcuda.dll");
        if (!h) return a;   /* non-NVIDIA machine — CUDA backend declines */

        a.CtxPushCurrent    = reinterpret_cast<MEB_PFN_cuCtxPushCurrent>(
                                  ::GetProcAddress(h, "cuCtxPushCurrent_v2"));
        a.CtxPopCurrent     = reinterpret_cast<MEB_PFN_cuCtxPopCurrent>(
                                  ::GetProcAddress(h, "cuCtxPopCurrent_v2"));
        a.ModuleLoadDataEx  = reinterpret_cast<MEB_PFN_cuModuleLoadDataEx>(
                                  ::GetProcAddress(h, "cuModuleLoadDataEx"));
        a.ModuleUnload      = reinterpret_cast<MEB_PFN_cuModuleUnload>(
                                  ::GetProcAddress(h, "cuModuleUnload"));
        a.ModuleGetFunction = reinterpret_cast<MEB_PFN_cuModuleGetFunction>(
                                  ::GetProcAddress(h, "cuModuleGetFunction"));
        a.LaunchKernel      = reinterpret_cast<MEB_PFN_cuLaunchKernel>(
                                  ::GetProcAddress(h, "cuLaunchKernel"));
        a.MemcpyHtoDAsync   = reinterpret_cast<MEB_PFN_cuMemcpyHtoDAsync>(
                                  ::GetProcAddress(h, "cuMemcpyHtoDAsync_v2"));

        a.loaded = a.CtxPushCurrent && a.CtxPopCurrent &&
                   a.ModuleLoadDataEx && a.ModuleUnload &&
                   a.ModuleGetFunction && a.LaunchKernel &&
                   a.MemcpyHtoDAsync;
        return a;
    }
};

/* Push AE's context for a scope of driver calls, pop on exit (the suite
   header's documented discipline for CUDA under AE). */
struct MEB_CudaCtxGuard {
    const MEB_CudaApi &api;
    bool pushed;
    explicit MEB_CudaCtxGuard(const MEB_CudaApi &a, CUcontext ctx)
        : api(a), pushed(false)
    {
        if (api.loaded && ctx)
            pushed = (api.CtxPushCurrent(ctx) == MEB_CUDA_SUCCESS);
    }
    ~MEB_CudaCtxGuard()
    {
        if (pushed) {
            CUcontext dummy = nullptr;
            api.CtxPopCurrent(&dummy);
        }
    }
};

#endif /* _WIN32 */
