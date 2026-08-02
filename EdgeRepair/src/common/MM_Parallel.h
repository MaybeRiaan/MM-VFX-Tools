/*  MM_Parallel.h — AE-free parallel-for primitive for the Edge Repair
    algorithm cores.

    The pure algorithm headers (MM_DistanceField.h, MM_Gaussian.h,
    MM_PullPush.h) must stay unit-testable with a plain compiler (no AE SDK),
    yet must ALSO fan their O(n) hot passes across cores inside AE. This header
    squares that circle with an INJECTED executor:

      - mm::ParallelFor(n, body) runs `body(threadIndex, i)` for i in [0,n).
      - By default it runs SERIALLY (threadIndex always 0) — that is the exact
        path the headless tests take, so the math is deterministic and byte-
        identical to the pre-Stage-5 serial code.
      - AE installs a backend (mm::SetParallelBackend) that routes the range
        through in_data->utils->iterate_generic, so AE's own scheduler owns the
        threads (MFR-safe: no std::thread oversubscription against AE's
        per-frame concurrency, and AE throttles the pool).

    WHY BYTE-IDENTICAL SURVIVES THREADING: every parallel body here writes
    ONLY outputs keyed off its own index i (a column, a row, a mip texel) —
    the work items are disjoint, so the result does not depend on execution
    order or thread count. Float accumulation order within one item is
    unchanged from the serial code. That is the discipline Stage 5 gates on.

    The backend is stored in a thread_local pointer so setting it on the
    render thread never races another concurrent render (MFR) — each AE worker
    thread that enters SmartRender installs its own. Reentrancy: ParallelFor
    runs the body on the CALLING thread's backend; nested ParallelFor falls
    back to serial for the inner level (iterate_generic is not reentrant), which
    is correct — we only ever parallelize the outer pass of each algorithm.

    No PF/SDK types in this header. The AE glue that adapts iterate_generic to
    the backend signature lives in MM_Common.cpp (which already includes the
    SDK), keeping this header pure.
*/

#pragma once

#include <functional>

namespace mm {

// A backend runs `body(threadIndex, i)` for every i in [0, n). It is free to
// call body concurrently from multiple threads, but must pass a stable, small
// threadIndex in [0, maxThreads) so callers can index per-thread scratch.
// Returning without having called body for some i is a contract violation.
using ParallelBackend =
    std::function<void(int n, const std::function<void(int /*threadIndex*/,
                                                       int /*i*/)> &body)>;

namespace detail_par {

// Per-thread so concurrent renders (MFR) never share/clobber it, and so a
// nested ParallelFor sees "no backend" and runs serial (see header note).
inline ParallelBackend &TLBackend()
{
    static thread_local ParallelBackend b;
    return b;
}

// Guards against nested parallelism: while a backend-driven ParallelFor is
// running, inner ParallelFor calls run serially on the worker thread.
inline bool &TLActive()
{
    static thread_local bool active = false;
    return active;
}

} // namespace detail_par

// Install (or clear, with a default-constructed ParallelBackend) the executor
// for the CURRENT thread. AE's SmartRender sets this at entry and clears it at
// exit; the headless tests never set it (serial path). Returns the previous
// backend so callers can restore it (RAII scope helper below does this).
inline ParallelBackend SetParallelBackend(ParallelBackend b)
{
    ParallelBackend prev = detail_par::TLBackend();
    detail_par::TLBackend() = std::move(b);
    return prev;
}

// Run body(threadIndex, i) for i in [0, n). Parallel via the installed
// backend when present and not already inside one; serial otherwise.
// The body must make only disjoint writes keyed off i (see header note).
template <typename Body>
inline void ParallelFor(int n, Body &&body)
{
    if (n <= 0) return;
    ParallelBackend &backend = detail_par::TLBackend();
    if (backend && !detail_par::TLActive()) {
        detail_par::TLActive() = true;
        // Wrap the templated body once in a std::function for the backend
        // boundary (per-pass cost, not per-index).
        std::function<void(int, int)> f =
            [&](int threadIndex, int i) { body(threadIndex, i); };
        backend(n, f);
        detail_par::TLActive() = false;
        return;
    }
    for (int i = 0; i < n; ++i) body(0, i);
}

// RAII: install a backend for the current scope, restore the previous on exit.
class ScopedParallelBackend {
public:
    explicit ScopedParallelBackend(ParallelBackend b)
        : prev_(SetParallelBackend(std::move(b))) {}
    ~ScopedParallelBackend() { SetParallelBackend(std::move(prev_)); }
    ScopedParallelBackend(const ScopedParallelBackend &) = delete;
    ScopedParallelBackend &operator=(const ScopedParallelBackend &) = delete;
private:
    ParallelBackend prev_;
};

} // namespace mm
