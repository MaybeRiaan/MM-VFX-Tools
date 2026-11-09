/*  mm_build_id.h — build provenance for the MM VFX Tools suite.
    Header-only, dependency-free, self-contained, shared by all five plugins.

    WHY THIS EXISTS
    ---------------
    Two roles, one mechanism.

    1. SUPPORT (release builds). When a user reports a problem, the only
       question that matters first is "which binary were you running". One
       request — send me the build log from your temp folder — answers it for
       every plugin in the suite at once, with no guessing from version
       numbers the user read off a UI.

    2. VERIFICATION (test builds). Two documented traps in this suite produce
       a PASS that is indistinguishable from a real result:

         STALE BINARY. AE holds a file lock on a loaded plugin, so a build that
         ran while AE was open compiles fine and silently does not install (see
         the mm_publish note in the suite CMakeLists.txt). The version quad in
         the log is then correct and the binary is still the old one.

         CACHE-SERVED RENDER. AE can answer a Save Frame As entirely from its
         frame cache, so an A/B can pass with ZERO plugin renders and difference
         to exactly 0.000e+00 (CLAUDE.md, "Verification traps").

       Neither is visible from the log alone, and nothing derived from the
       SOURCE can catch either one — only the identity of the binary AE
       actually loaded can. tools/verify-build-id.ps1 turns both into
       assertions against the line this header writes.

    Because role 1 ships, nothing here may sit behind a plugin's temporary
    diagnostic switch. Announce() is unconditional, writes exactly one line per
    plugin load, self-caps the log, and needs no logging infrastructure from the
    host plugin — which is also what makes the per-plugin wiring two lines.

    HOW IT WORKS
    ------------
    At first call it locates the module containing this code (GetModuleHandleEx
    on Windows, dladdr on macOS), stats it, and derives a 32-bit id from
    path + size + mtime. The id is cheap enough to print on every render line,
    so a harness can prove that every render in a run came from ONE loaded
    binary, and that this binary is the one named in the [build] line.

    The id is deliberately NOT a content hash. A content hash would have to be
    reproduced bit-exactly on the harness side; instead the [build] line reports
    the real loaded path and the harness hashes that path itself with
    Get-FileHash. The id's only job is internal consistency across lines.

    USAGE
    -----
        #include "../../common/mm_build_id.h"

        // in GlobalSetup, once per load — unconditional, ships:
        mmbuild::Announce("GrainTransfer", MAJOR_VERSION, MINOR_VERSION,
                          BUG_VERSION, BUILD_VERSION);

        // optional, test builds only: attribute each render to that binary by
        // appending  id=%08x  to an existing per-render log line:
        //     ..., mmbuild::ShortId(), ...

    Relative-path include is deliberate: it keeps every project building
    standalone from its own folder with no CMake change, which is the suite's
    standing contract.
*/

#ifndef MM_BUILD_ID_H
#define MM_BUILD_ID_H

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace mmbuild {

// One shared file for the whole suite: a single support request returns every
// plugin the user has loaded, with versions, rather than five separate files.
#ifndef MM_BUILD_LOG_NAME
#define MM_BUILD_LOG_NAME "MM_VFX_Tools_build.log"
#endif

// Ships, so it must not grow without bound. One line per plugin load is tiny,
// but AE sessions accumulate; past this the file is rewritten rather than
// appended. 256 KB is thousands of loads.
#ifndef MM_BUILD_LOG_MAX_BYTES
#define MM_BUILD_LOG_MAX_BYTES (256 * 1024)
#endif

struct Id {
    char               path[1024];
    unsigned long long size;
    long long          mtime;
    unsigned           id;
};

// FNV-1a, 32-bit. Short enough to sit on every render line without bloating
// the log; collision risk is irrelevant because the id is only ever compared
// against other ids in the SAME log file.
inline unsigned Fnv1a(const void *data, size_t n, unsigned h = 2166136261u)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// The temp directory — the only OS-dependent part besides module lookup.
// Windows has no $TMPDIR; macOS has no GetTempPathA, and its TMPDIR is a
// per-user path under /var/folders that already ends in a separator (as does
// GetTempPathA's result, and as does the /tmp/ fallback when unset).
inline std::string TempDir()
{
#ifdef _WIN32
    char dir[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH - 64) return std::string();
    return std::string(dir, n);
#else
    const char *t = std::getenv("TMPDIR");
    return std::string((t && *t) ? t : "/tmp/");
#endif
}

namespace detail {

// Anchor whose address lies inside this module, so the OS can map it back to
// the file on disk. A function, not a variable: with /OPT:ICF or -dead_strip
// an unused variable can be folded away, while a static-inline function's
// address is stable per module.
inline void Anchor() {}

inline Id Compute()
{
    Id r;
    std::memset(&r, 0, sizeof(r));
    std::snprintf(r.path, sizeof(r.path), "%s", "<unknown>");

#ifdef _WIN32
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&Anchor, &mod) && mod) {
        char buf[1024];
        const DWORD n = GetModuleFileNameA(mod, buf, (DWORD)sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            std::memcpy(r.path, buf, (size_t)n + 1);
            struct _stat64 st;
            if (_stat64(r.path, &st) == 0) {
                r.size  = (unsigned long long)st.st_size;
                r.mtime = (long long)st.st_mtime;
            }
        }
    }
#else
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (dladdr((const void *)&Anchor, &info) && info.dli_fname) {
        std::snprintf(r.path, sizeof(r.path), "%s", info.dli_fname);
        struct stat st;
        if (stat(r.path, &st) == 0) {
            r.size  = (unsigned long long)st.st_size;
            r.mtime = (long long)st.st_mtime;
        }
    }
#endif

    unsigned h = Fnv1a(r.path, std::strlen(r.path));
    h = Fnv1a(&r.size,  sizeof(r.size),  h);
    h = Fnv1a(&r.mtime, sizeof(r.mtime), h);
    r.id = h;
    return r;
}

inline void LocalStamp(char *buf, size_t cap)
{
    const std::time_t t = std::time(NULL);
    std::tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    if (std::strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tmv) == 0)
        std::snprintf(buf, cap, "%s", "unknown");
}

} // namespace detail

// Thread-safe once-init (C++11 magic statics). AE renders on many threads, so
// the first caller may well be a render thread rather than GlobalSetup.
inline const Id &Get()
{
    static const Id k = detail::Compute();
    return k;
}

inline unsigned ShortId() { return Get().id; }

// The standard [build] line, identical across all five plugins so one parser
// reads them all. `path` goes LAST and unquoted: AE plugin paths contain
// spaces, and putting it last makes the harness regex "everything after
// path=" instead of a quoting problem. Fields are name=value so new ones can
// be added without breaking an existing reader.
inline const char *FormatLine(char *buf, size_t cap,
                              const char *plugin, const char *version)
{
    const Id &k = Get();
    char when[32];
    detail::LocalStamp(when, sizeof(when));
    std::snprintf(buf, cap,
                  "[build] plugin=%s version=%s id=%08x time=%s size=%llu "
                  "mtime=%lld path=%s",
                  plugin, version, k.id, when,
                  (unsigned long long)k.size, (long long)k.mtime, k.path);
    return buf;
}

// Append one line to the suite build log. Self-capping (see
// MM_BUILD_LOG_MAX_BYTES). Silent on every failure: provenance logging must
// never be able to affect a render.
inline void WriteLine(const char *line)
{
    const std::string dir = TempDir();
    if (dir.empty() || !line) return;
    const std::string path = dir + MM_BUILD_LOG_NAME;

    // "a" normally; "w" once the file outgrows the cap, which is the whole
    // rotation policy — one line per load does not justify anything cleverer.
    bool rotate = false;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) == 0 && st.st_size > MM_BUILD_LOG_MAX_BYTES)
        rotate = true;
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > MM_BUILD_LOG_MAX_BYTES)
        rotate = true;
#endif
    const char *mode = rotate ? "w" : "a";

    FILE *f = NULL;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), mode) != 0 || !f) return;
#else
    f = std::fopen(path.c_str(), mode);
    if (!f) return;
#endif
    // One fwrite of one pre-assembled buffer, so concurrent writers from
    // different plugins cannot interleave halves of a line.
    char out[1400];
    const int n = std::snprintf(out, sizeof(out), "%s\n", line);
    if (n > 0) std::fwrite(out, 1, (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1), f);
    std::fclose(f);
}

// Call once from PF_Cmd_GLOBAL_SETUP. Unconditional by design — this is the
// shipping support path, not a debug aid. Guarded so a repeated GLOBAL_SETUP
// within one load cannot spam the log; statics reset on reload, which is
// exactly the granularity wanted.
inline void Announce(const char *plugin, const char *version)
{
    static bool done = false;
    if (done) return;
    done = true;
    char line[1300];
    WriteLine(FormatLine(line, sizeof(line), plugin, version));
}

// Version-quad overload, so each plugin passes its own defines and the string
// can never drift from them.
inline void Announce(const char *plugin, int major, int minor, int bug, int build)
{
    char v[48];
    std::snprintf(v, sizeof(v), "%d.%d.%d.%d", major, minor, bug, build);
    Announce(plugin, v);
}

} // namespace mmbuild

#endif // MM_BUILD_ID_H
