/*  gpu_parity_test.cpp — GPU kernel math validated WITHOUT a GPU.

    The .kernel bodies in src/gpu/kernels/ are platform-free by rule
    (docs/gpu-kernel-rules.md §0), which has a useful consequence: their
    arithmetic can be transcribed into plain C and diffed against the CPU
    reference on this machine, with no DX12 device, no AE host, and no Mac.

    That is what this file does. It catches ALGORITHM error now, so that when
    the DX12 host lands, any remaining mismatch is unambiguously PLUMBING —
    and when Metal is written later, the math is already known-good.

    IMPORTANT: the emulations below must stay a faithful transcription of the
    .kernel bodies. If you change a kernel, change it here. The whole value of
    this file is that it is the same arithmetic, not a re-derivation. It links
    the REAL src/blur.cpp and the REAL src/fs_core.h as the reference side.
*/

#define NOMINMAX
#include "blur.h"
#include "fs_core.h"
#include "guided.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char *name, double detail)
{
    if (ok) { ++g_pass; std::printf("  PASS  %s (max-abs %.3e)\n", name, detail); }
    else    { ++g_fail; std::printf("  FAIL  %s (max-abs %.3e)\n", name, detail); }
}

// Deterministic pseudo-random fill — same data every run so a failure is
// reproducible. Alpha deliberately includes exact 0 and near-0 values to
// exercise the coverage-epsilon branches.
static void FillRandom(std::vector<float> &buf, unsigned seed, bool someZeroAlpha)
{
    unsigned s = seed;
    auto next = [&s]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.0f; };
    for (size_t i = 0; i < buf.size(); i += 4) {
        float a = next();
        if (someZeroAlpha && (i / 4) % 7 == 0) a = 0.0f;
        buf[i + 0] = a;
        // Premultiplied: colour never exceeds alpha.
        buf[i + 1] = next() * a;
        buf[i + 2] = next() * a;
        buf[i + 3] = next() * a;
    }
}

// ---------------------------------------------------------------------------
// Transcription of fs_gauss.kernel. One invocation == one GPU thread.
// Note the tap-range narrowing (i0/i1): SKIP out-of-range, never clamp.
// ---------------------------------------------------------------------------
static void EmulateGaussPass(const float *src, const float *kern, float *dst,
                             int W, int H, int R, int horizontal)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int oo = (y * W + x) * 4;
            if (R == 0) {
                dst[oo+0]=src[oo+0]; dst[oo+1]=src[oo+1];
                dst[oo+2]=src[oo+2]; dst[oo+3]=src[oo+3];
                continue;
            }
            int i0, i1;
            if (horizontal) {
                i0 = (x - R < 0)     ? -x         : -R;
                i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
            } else {
                i0 = (y - R < 0)     ? -y         : -R;
                i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
            }
            float a=0.f, r=0.f, g=0.f, b=0.f;
            for (int i = i0; i <= i1; ++i) {
                const float w = kern[i + R];
                const int o = horizontal ? ((y * W + (x + i)) * 4)
                                         : (((y + i) * W + x) * 4);
                a += w * src[o+0]; r += w * src[o+1];
                g += w * src[o+2]; b += w * src[o+3];
            }
            dst[oo+0]=a; dst[oo+1]=r; dst[oo+2]=g; dst[oo+3]=b;
        }
    }
}

// Rebuilds the weights the way blur.cpp does — in DOUBLE, then cast to float
// (rules §3.1). This is what the host will upload; the GPU never rebuilds them.
static void BuildKernelLikeCPU(double sigma, int R, std::vector<float> &k)
{
    k.resize((size_t)(2 * R + 1));
    double sum = 0.0;
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    for (int i = -R; i <= R; ++i) {
        const double w = std::exp(-(double)(i * i) * inv2s2);
        k[(size_t)(i + R)] = (float)w;
        sum += w;
    }
    const float inv = (float)(1.0 / sum);
    for (float &w : k) w *= inv;
}

// ---------------------------------------------------------------------------
// Transcription of fs_compose.kernel (Result view, no offset, matte == 1).
// ---------------------------------------------------------------------------
static void EmulateComposeResult(const float *src, const float *lo, float *dst,
                                 int W, int H, float texGain, float mix)
{
    const float EPS = 1e-8f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int o = (y * W + x) * 4;
            const float a = src[o+0], la = lo[o+0];
            float s0,s1,s2, l0,l1,l2;
            if (a > EPS) { const float inva=1.f/a;
                s0=src[o+1]*inva; s1=src[o+2]*inva; s2=src[o+3]*inva; }
            else { s0=s1=s2=0.f; }
            if (la > EPS) { const float invla=1.f/la;
                l0=lo[o+1]*invla; l1=lo[o+2]*invla; l2=lo[o+3]*invla; }
            else { l0=s0; l1=s1; l2=s2; }

            if (a > EPS) {
                const float m = 1.0f * mix;
                const float h0=s0-l0, h1=s1-l1, h2=s2-l2;
                const float p0=l0+texGain*h0, p1=l1+texGain*h1, p2=l2+texGain*h2;
                const float o0=s0+(p0-s0)*m, o1=s1+(p1-s1)*m, o2=s2+(p2-s2)*m;
                dst[o+0]=a; dst[o+1]=o0*a; dst[o+2]=o1*a; dst[o+3]=o2*a;
            } else {
                dst[o+0]=a; dst[o+1]=src[o+1]; dst[o+2]=src[o+2]; dst[o+3]=src[o+3];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Transcription of fs_guided.kernel. Five stages, dispatched in guided.cpp's
// order. NOTE the deliberate divergence: the box sum here is a DIRECT per-pixel
// window sum (GPU-shaped, fully parallel), not the CPU's serial sliding window
// — accumulated in double, which measurement shows is bit-exact against it
// while a float accumulator is not. See the header comment in fs_guided.kernel.
// ---------------------------------------------------------------------------
static void EmulateGuidedBox(const float *src, float *dst,
                             int W, int H, int R, int horizontal)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double a0=0, a1=0, a2=0, a3=0;      // ACCUM_T
            int i0, i1;
            if (horizontal) {
                i0 = (x - R < 0)     ? -x         : -R;
                i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
            } else {
                i0 = (y - R < 0)     ? -y         : -R;
                i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
            }
            for (int i = i0; i <= i1; ++i) {
                const int o = horizontal ? ((y * W + (x + i)) * 4)
                                         : (((y + i) * W + x) * 4);
                a0 += src[o+0]; a1 += src[o+1]; a2 += src[o+2]; a3 += src[o+3];
            }
            const int o = (y * W + x) * 4;
            dst[o+0]=(float)a0; dst[o+1]=(float)a1;
            dst[o+2]=(float)a2; dst[o+3]=(float)a3;
        }
    }
}

static void EmulateGuidedBoxSeparable(const float *src, float *tmp, float *dst,
                                      int W, int H, int rx, int ry)
{
    EmulateGuidedBox(src, tmp, W, H, rx, 1);
    EmulateGuidedBox(tmp, dst, W, H, ry, 0);
}

static void EmulateGuided(const float *src, float *dst, int W, int H,
                          int rx, int ry, float eps)
{
    const size_t N4 = (size_t)W * H * 4;
    const float WEPS = 1e-6f, AEPS = 1e-8f;

    if (rx == 0 && ry == 0) {
        for (size_t i = 0; i < N4; ++i) dst[i] = src[i];
        return;
    }
    if (eps < 1e-6f) eps = 1e-6f;

    std::vector<float> s1(N4), s2(N4), sa(N4), sb(N4), tmp(N4);

    // stage 1: box(src) -> s1
    EmulateGuidedBoxSeparable(src, tmp.data(), s1.data(), W, H, rx, ry);

    // stage 2: squares -> s2
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float a = src[o+0];
        s2[o+0] = a;
        if (a > AEPS) { const float inva = 1.f/a;
            s2[o+1]=src[o+1]*src[o+1]*inva;
            s2[o+2]=src[o+2]*src[o+2]*inva;
            s2[o+3]=src[o+3]*src[o+3]*inva;
        } else { s2[o+1]=s2[o+2]=s2[o+3]=0.f; }
    }
    EmulateGuidedBoxSeparable(s2.data(), tmp.data(), s2.data(), W, H, rx, ry);

    // stage 3: coefficients -> sa/sb
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float a = src[o+0], N = s1[o+0];
        if (N > WEPS) {
            const float invN = 1.f/N;
            sa[o+0]=a; sb[o+0]=a;
            for (int c = 1; c < 4; ++c) {
                const float mean = s1[o+c]*invN;
                float var = s2[o+c]*invN - mean*mean;
                if (var < 0.f) var = 0.f;
                const float A = var/(var+eps);
                sa[o+c] = a*A;
                sb[o+c] = a*(mean*(1.f-A));
            }
        } else {
            for (int c = 0; c < 4; ++c) { sa[o+c]=0.f; sb[o+c]=0.f; }
        }
    }
    EmulateGuidedBoxSeparable(sa.data(), tmp.data(), sa.data(), W, H, rx, ry);
    EmulateGuidedBoxSeparable(sb.data(), tmp.data(), sb.data(), W, H, rx, ry);

    // stage 4: final
    const float invCount = 1.f / (float)((2*rx+1)*(2*ry+1));
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float N2 = sa[o+0];
        if (N2 > WEPS) {
            const float invN2 = 1.f/N2;
            const float abar = N2*invCount;
            const float a = src[o+0];
            const float inva = (a > AEPS) ? 1.f/a : 0.f;
            dst[o+0] = abar;
            for (int c = 1; c < 4; ++c) {
                const float p = src[o+c]*inva;
                const float q = sa[o+c]*invN2*p + sb[o+c]*invN2;
                dst[o+c] = q*abar;
            }
        } else {
            for (int c = 0; c < 4; ++c) dst[o+c] = 0.f;
        }
    }
}

static double MaxAbsDiff(const std::vector<float> &a, const std::vector<float> &b)
{
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

int main()
{
    std::printf("\nMM FreqSep - GPU kernel parity (CPU-emulated, no device)\n\n");

    const int W = 64, H = 48;
    const size_t N = (size_t)W * H * 4;

    std::vector<float> src(N), ref(N), tmp(N), gpu(N), gtmp(N);
    FillRandom(src, 12345u, /*someZeroAlpha*/ true);

    // --- Gaussian: emulated two-pass vs the REAL blur.cpp ------------------
    // Several sigmas, including one small enough that the buffer edge clamps
    // the tap range on most pixels (that is where skip-vs-clamp shows up).
    const double sigmas[] = { 0.8, 2.5, 7.0, 15.0 };
    for (double sigma : sigmas) {
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);

        const int R = (int)fs::GaussianSupport(sigma);
        std::vector<float> k;
        BuildKernelLikeCPU(sigma, R, k);
        EmulateGaussPass(src.data(),  k.data(), gtmp.data(), W, H, R, 1);
        EmulateGaussPass(gtmp.data(), k.data(), gpu.data(),  W, H, R, 0);

        char name[96];
        std::snprintf(name, sizeof name, "fs_gauss.kernel vs blur.cpp, sigma=%.1f", sigma);
        const double d = MaxAbsDiff(ref, gpu);
        check(d <= 1e-5, name, d);
    }

    // --- Anisotropic sigmas (non-square pixel aspect / downsample) ----------
    {
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, 6.0, 2.0);
        const int Rx = (int)fs::GaussianSupport(6.0);
        const int Ry = (int)fs::GaussianSupport(2.0);
        std::vector<float> kx, ky;
        BuildKernelLikeCPU(6.0, Rx, kx);
        BuildKernelLikeCPU(2.0, Ry, ky);
        EmulateGaussPass(src.data(),  kx.data(), gtmp.data(), W, H, Rx, 1);
        EmulateGaussPass(gtmp.data(), ky.data(), gpu.data(),  W, H, Ry, 0);
        const double d = MaxAbsDiff(ref, gpu);
        check(d <= 1e-5, "fs_gauss.kernel anisotropic sigma 6.0/2.0", d);
    }

    // --- Skip-don't-clamp: prove the edge behaviour actually differs --------
    // A control showing that clamp-sampling (the reflexive GPU choice) would
    // NOT match. If this "fails", the two behaviours have become identical and
    // the invariant is no longer being tested by the cases above.
    {
        const double sigma = 7.0;
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);
        const int R = (int)fs::GaussianSupport(sigma);
        std::vector<float> k; BuildKernelLikeCPU(sigma, R, k);

        // Deliberately WRONG variant: clamp the tap index to the edge.
        std::vector<float> bad(N), badtmp(N);
        for (int pass = 0; pass < 2; ++pass) {
            const float *in = (pass == 0) ? src.data() : badtmp.data();
            float *out      = (pass == 0) ? badtmp.data() : bad.data();
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                float a=0,r=0,g=0,b=0;
                for (int i = -R; i <= R; ++i) {
                    const float w = k[i + R];
                    int sx = x, sy = y;
                    if (pass == 0) { sx = x + i; sx = sx < 0 ? 0 : (sx > W-1 ? W-1 : sx); }
                    else           { sy = y + i; sy = sy < 0 ? 0 : (sy > H-1 ? H-1 : sy); }
                    const int o = (sy * W + sx) * 4;
                    a += w*in[o+0]; r += w*in[o+1]; g += w*in[o+2]; b += w*in[o+3];
                }
                const int oo = (y * W + x) * 4;
                out[oo+0]=a; out[oo+1]=r; out[oo+2]=g; out[oo+3]=b;
            }
        }
        const double d = MaxAbsDiff(ref, bad);
        check(d > 1e-4, "control: clamp-sampling edges DOES diverge (invariant is live)", d);
    }

    // --- Compose: emulated kernel vs fs_core.h -----------------------------
    {
        const double sigma = 5.0;
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);

        struct { float gain, mix; const char *name; } cases[] = {
            { 1.0f, 1.0f, "compose identity gain=1 mix=1 -> out==src" },
            { 1.6f, 1.0f, "compose texture gain=1.6" },
            { 0.0f, 1.0f, "compose texture gain=0 (flatten)" },
            { 2.0f, 0.35f,"compose gain=2.0 mix=0.35" },
        };

        for (auto &c : cases) {
            EmulateComposeResult(src.data(), ref.data(), gpu.data(), W, H, c.gain, c.mix);

            // Reference side: drive the REAL fs::RecombineChannel.
            std::vector<float> cref(N);
            const float EPS = 1e-8f;
            for (int i = 0; i < W * H; ++i) {
                const int o = i * 4;
                const float a = src[o+0], la = ref[o+0];
                float s[3], l[3];
                if (a > EPS) { for (int c2=0;c2<3;++c2) s[c2]=src[o+1+c2]/a; }
                else { s[0]=s[1]=s[2]=0.f; }
                if (la > EPS) { for (int c2=0;c2<3;++c2) l[c2]=ref[o+1+c2]/la; }
                else { l[0]=s[0]; l[1]=s[1]; l[2]=s[2]; }
                if (a > EPS) {
                    cref[o+0]=a;
                    for (int c2=0;c2<3;++c2)
                        cref[o+1+c2] = fs::RecombineChannel(s[c2], l[c2], c.gain, c.mix) * a;
                } else {
                    cref[o+0]=a; cref[o+1]=src[o+1]; cref[o+2]=src[o+2]; cref[o+3]=src[o+3];
                }
            }
            const double d = MaxAbsDiff(cref, gpu);
            check(d <= 1e-6, c.name, d);
        }

        // Identity, end to end on PREMULTIPLIED values. The associativity of
        // RecombineChannel makes the STRAIGHT-colour result exact at gain=1,
        // mix=1 — but compose then round-trips through straight and back
        // (s = src/a ... out = s*a), and that divide/multiply pair costs about
        // one ulp. So the bound here is ~1 ulp, NOT zero.
        //
        // This is not GPU-specific: the CPU compose loop does the identical
        // round-trip, which is why tests/identity_test.cpp asserts <= 1e-6 on
        // RecombineChannel (straight colour, pre-premultiply) rather than
        // exactness. Both paths agree; the residue is float, not algorithm.
        EmulateComposeResult(src.data(), ref.data(), gpu.data(), W, H, 1.0f, 1.0f);
        const double d = MaxAbsDiff(src, gpu);
        check(d <= 2e-7, "compose identity: out==src to ~1ulp (premult round-trip)", d);
    }

    // --- Guided filter: emulated 5-stage pipeline vs the REAL guided.cpp ----
    // Radii from GuidedRadius() at realistic sigmas, plus a large one where the
    // float-accumulator hazard would bite hardest. eps spans the slider range.
    {
        struct { double sigma; float eps; } gcases[] = {
            { 2.0,  1e-4f }, { 8.0,  1e-4f }, { 8.0, 1e-2f },
            { 8.0,  1e-6f },                        // eps floor
            { 20.0, 1e-3f },                        // large radius
        };
        for (auto &c : gcases) {
            const int r = (int)fs::GuidedRadius(c.sigma);
            std::vector<float> gref(N), ggpu(N);
            const PF_Err e = fs::GuidedFilterARGB(nullptr, src.data(), gref.data(),
                                                  W, H, r, r, c.eps);
            if (e != PF_Err_NONE) { std::printf("  FAIL  guided alloc\n"); ++g_fail; continue; }
            EmulateGuided(src.data(), ggpu.data(), W, H, r, r, c.eps);

            char name[128];
            std::snprintf(name, sizeof name,
                          "fs_guided.kernel vs guided.cpp, sigma=%.1f r=%d eps=%.0e",
                          c.sigma, r, (double)c.eps);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d <= 1e-5, name, d);
        }

        // Anisotropic radii (non-square downsample).
        {
            const int rx = (int)fs::GuidedRadius(9.0), ry = (int)fs::GuidedRadius(3.0);
            std::vector<float> gref(N), ggpu(N);
            fs::GuidedFilterARGB(nullptr, src.data(), gref.data(), W, H, rx, ry, 1e-4f);
            EmulateGuided(src.data(), ggpu.data(), W, H, rx, ry, 1e-4f);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d <= 1e-5, "fs_guided.kernel anisotropic radii", d);
        }

        // r == 0 must be an exact copy (identity chain, guided.cpp:215-219).
        {
            std::vector<float> gref(N), ggpu(N);
            fs::GuidedFilterARGB(nullptr, src.data(), gref.data(), W, H, 0, 0, 1e-4f);
            EmulateGuided(src.data(), ggpu.data(), W, H, 0, 0, 1e-4f);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d == 0.0, "fs_guided.kernel r=0: exact copy", d);
        }

        // CONTROL: the GPU box sum diverges from the CPU's serial sliding
        // window if accumulated in FLOAT instead of ACCUM_T/double. This is why
        // the prologue routes accumulation through ACCUM_T (and why the Metal
        // prologue needs a Kahan sum — Apple GPUs have no double). If this
        // stops failing-to-match, the hazard is no longer being tested.
        {
            const int R = 25;
            std::vector<float> boxRefD(N), boxF(N), tmpb(N);
            EmulateGuidedBoxSeparable(src.data(), tmpb.data(), boxRefD.data(), W, H, R, R);

            // float-accumulator variant of the same direct sum
            auto boxFloat = [&](const float *in, float *out, int horiz) {
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    float a0=0,a1=0,a2=0,a3=0;
                    int i0,i1;
                    if (horiz) { i0=(x-R<0)?-x:-R; i1=(x+R>W-1)?(W-1-x):R; }
                    else       { i0=(y-R<0)?-y:-R; i1=(y+R>H-1)?(H-1-y):R; }
                    for (int i=i0;i<=i1;++i) {
                        const int o = horiz ? ((y*W+(x+i))*4) : (((y+i)*W+x)*4);
                        a0+=in[o+0]; a1+=in[o+1]; a2+=in[o+2]; a3+=in[o+3];
                    }
                    const int o=(y*W+x)*4;
                    out[o+0]=a0; out[o+1]=a1; out[o+2]=a2; out[o+3]=a3;
                }
            };
            std::vector<float> t2(N);
            boxFloat(src.data(), t2.data(), 1);
            boxFloat(t2.data(), boxF.data(), 0);
            const double d = MaxAbsDiff(boxRefD, boxF);
            check(d > 0.0, "control: float box accumulator DOES drift (ACCUM_T earns its keep)", d);
        }
    }

    std::printf("\n  %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
