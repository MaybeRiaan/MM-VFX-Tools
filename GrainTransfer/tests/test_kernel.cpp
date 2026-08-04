/*  test_kernel.cpp — the GPU kernel BODY, run and diffed against the CPU
    reference. AE-free: no GPU, no CUDA toolkit, no DX12 device, no AE host.

    This does not transcribe the kernel — it COMPILES IT. src/gpu/cuda/
    gt_kernel_args.h pulls the real gt_stamp.kernel through
    gt_prologue_host.h, which is the same vocabulary gt_prologue_cuda.h
    defines with CUDA spellings, so `gtk_gt_stamp::Args{...}.run(x, y)`
    executes the shipped kernel source on the CPU. A transcription can rot
    silently; this cannot — if the kernel changes, this test runs the change.

    WHY THIS FILE HAD TO EXIST BEFORE THE PORT COULD BE TRUSTED. The other two
    gates here (test_core.cpp, test_realfootage.cpp) are excellent, and they
    are DSP gates: they know nothing about a GPU path, so a green run from
    them says nothing whatever about GPU parity. This is the regression gate
    for the port, and it is the same arrangement MM_Qualify now uses.

    What each case proves, since both sides call the same gt_core.h for the
    per-pixel math: the kernel's INDEXING, its buffer-slot order, its
    PARAM_BLOCK layout (including the 23-float response block round-tripping
    through gt::PackResponse -> GT_RESP_LOAD), its BGRA<->ARGB swizzle, and
    its pitch handling. Those are exactly the places a kernel diverges from
    the CPU it mirrors.

    Tolerance is EXACT (0.0). Same source, same arithmetic, same order — any
    difference at all is a real defect (AE_GPU_Effects.md §4.3). The <= 1e-5
    tolerance in gpu-kernel-rules §3.6 is for real silicon, where fma
    contraction legitimately differs; it has no business here.

    Exit 0 = all pass.
*/

#include "GT_Colour.h"
#include "GT_Grain.h"
#include "GT_Random.h"
#include "GT_Response.h"
#include "GT_Stamp.h"

#include "../gpu/cuda/gt_kernel_args.h"

#include <cmath>
#include <cstdio>
#include <vector>

using std::vector;
using namespace gtgpu;

static int g_fail = 0, g_total = 0;

static void check(const char *name, float d)
{
    ++g_total;
    const bool ok = (d == 0.f);
    std::printf("  %-52s max-abs = %.3e  %s\n", name, d, ok ? "OK" : "FAIL");
    if (!ok) ++g_fail;
}

// ---------------------------------------------------------------------------
// Deterministic fixtures.
// ---------------------------------------------------------------------------

// Zero-mean unit-ish tiles. Contents are irrelevant to parity — what matters
// is that every tap index the addressing produces lands on a distinct value,
// so a wrong index cannot coincidentally agree.
static gt::TileBank makeBank(int n, int count, uint64_t seed)
{
    gt::TileBank b;
    b.n = n;
    b.count = count;
    b.tiles.resize((size_t)count * 3 * n * n);
    gt::Rng rng(seed);
    for (size_t i = 0; i < b.tiles.size(); ++i)
        b.tiles[i] = rng.nextFloat() * 2.f - 1.f;
    return b;
}

// Band mids deliberately span NEGATIVE luma: the LogFromLinear path routinely
// produces L < 0 (the real-footage gate measures band mids at -4.1 on a
// scene-linear plate), and EvalSigma's end clamps must be exercised on both
// sides of the range.
static gt::ResponseCurve makeResp(uint64_t seed)
{
    gt::ResponseCurve r;
    gt::Rng rng(seed);
    for (int b = 0; b < gt::ResponseCurve::kBands; ++b)
        r.lumaMid[b] = -1.0f + 0.6f * (float)b;
    for (int c = 0; c < 3; ++c) {
        for (int b = 0; b < gt::ResponseCurve::kBands; ++b)
            r.sigma[c][b] = 0.002f + 0.02f * rng.nextFloat();
        r.flatSigma[c] = 0.004f + 0.01f * rng.nextFloat();
    }
    r.valid = true;
    return r;
}

// Packed premultiplied ARGB. Alpha reaches EXACT zero and a value just under
// the 1e-6 contract epsilon so both passthrough branches are hit; colours run
// negative (wide-gamut) and well above 1.0 (HDR) so nothing assumes [0,1].
static void fillARGB(vector<float> &buf, int W, int H, uint64_t seed)
{
    gt::Rng rng(seed);
    for (int i = 0; i < W * H; ++i) {
        float a;
        switch (i % 7) {
            case 0:  a = 0.f;    break;   // exact zero  -> untouched
            case 1:  a = 1e-7f;  break;   // below epsilon -> untouched
            case 2:  a = 1.f;    break;
            default: a = rng.nextFloat();
        }
        buf[(size_t)i * 4 + 0] = a;
        for (int c = 1; c < 4; ++c) {
            const float v = rng.nextFloat() * 3.f - 0.5f;   // [-0.5, 2.5)
            buf[(size_t)i * 4 + c] = v * a;                 // premultiplied
        }
    }
}

// ---------------------------------------------------------------------------
// One parity case: CPU StampRows over packed ARGB vs the kernel over pitched
// BGRA worlds. The pitch is deliberately NOT W*4 — a kernel that ignores it
// still passes on a tight buffer.
// ---------------------------------------------------------------------------
struct Case {
    const char  *name;
    gt::Encode   enc;
    bool         preview;
    float        invScale;
    bool         flipX, flipY;
    int          tileIndex;
    int          offX, offY;
    float        amount, trim;
};

static float runCase(const Case &k, const gt::TileBank &bank,
                     const gt::ResponseCurve &resp, int W, int H)
{
    vector<float> img((size_t)W * H * 4);
    fillARGB(img, W, H, 0x9E37u);

    gt::StampParams sp;
    sp.tileIndex = k.tileIndex;
    sp.offX = k.offX;
    sp.offY = k.offY;
    sp.flipX = k.flipX;
    sp.flipY = k.flipY;
    sp.invScale = k.invScale;
    sp.amount = k.amount;
    sp.trim = k.trim;
    sp.encode = k.enc;
    sp.previewGrainOnly = k.preview;

    // --- CPU reference (in place, packed ARGB) ---
    vector<float> cpu = img;
    gt::StampRows(cpu.data(), W, H, 0, H, bank, resp, sp);

    // --- kernel (pitched BGRA worlds, separate src and dst) ---
    const int pitch = W * 4 + 7;            // odd padding on purpose
    vector<float> src((size_t)pitch * H, -12345.f);
    vector<float> dst((size_t)pitch * H, -54321.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c)     // ARGB -> BGRA
                src[(size_t)y * pitch + x * 4 + c] = img[((size_t)y * W + x) * 4 + (3 - c)];

    float packed[GT_RESP_FLOATS];
    gt::PackResponse(resp, packed);

    ArgsStamp a{};
    a.gGtSrc   = src.data();
    a.gGtTiles = bank.tiles.data();
    a.gGtDst   = dst.data();
    a.gGtW = W;             a.gGtH = H;
    a.gGtSrcPitch = pitch;  a.gGtDstPitch = pitch;
    a.gGtN = bank.n;
    a.gGtTileIndex = sp.tileIndex;
    a.gGtOffX = sp.offX;    a.gGtOffY = sp.offY;
    a.gGtFlipX = sp.flipX ? 1 : 0;
    a.gGtFlipY = sp.flipY ? 1 : 0;
    a.gGtExact = std::fabs(sp.invScale - 1.f) < 1e-6f ? 1 : 0;
    a.gGtEncode = (int)sp.encode;
    a.gGtPreview = sp.previewGrainOnly ? 1 : 0;
    a.gGtInvScale = sp.invScale;
    a.gGtAmount = sp.amount;
    a.gGtTrim = sp.trim;
    a.gGtPreviewBase = GT_PreviewBase((int)sp.encode);
    GT_StoreResponse(a, packed);

    GT_RunGrid(a, W, H);

    // --- compare, unswizzling back to ARGB ---
    float m = 0.f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float got = dst[(size_t)y * pitch + x * 4 + (3 - c)];
                const float ref = cpu[((size_t)y * W + x) * 4 + c];
                const float d = std::fabs(got - ref);
                if (d > m) m = d;
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== Grain Transfer — kernel vs CPU parity (exact) ===\n\n");

    const int W = 61, H = 37;               // not multiples of the 8x8 group
    const gt::TileBank      bank = makeBank(16, 4, 0xC0FFEEull);
    const gt::ResponseCurve resp = makeResp(0xBEEFull);

    // invScale 1.0 takes the exact integer-wrap path; anything else takes
    // bilinear-wrap. flips drive the offsets NEGATIVE, which is the case a
    // naive `%` gets wrong.
    const Case cases[] = {
        { "display, exact, no flip",        gt::Encode::Display,       false, 1.0f,  false, false, 0, 5,  9,  1.0f, 1.0f },
        { "display, exact, flipX",          gt::Encode::Display,       false, 1.0f,  true,  false, 1, 3,  2,  1.0f, 1.0f },
        { "display, exact, flipY",          gt::Encode::Display,       false, 1.0f,  false, true,  2, 0,  0,  1.0f, 1.0f },
        { "display, exact, flipXY",         gt::Encode::Display,       false, 1.0f,  true,  true,  3, 15, 15, 0.6f, 0.4f },
        { "display, bilinear 0.7",          gt::Encode::Display,       false, 0.7f,  false, false, 0, 4,  6,  1.0f, 1.0f },
        { "display, bilinear 1.5, flipXY",  gt::Encode::Display,       false, 1.5f,  true,  true,  1, 7,  1,  1.3f, 0.5f },
        { "log, exact, no flip",            gt::Encode::LogFromLinear, false, 1.0f,  false, false, 2, 8,  3,  1.0f, 1.0f },
        { "log, exact, flipXY",             gt::Encode::LogFromLinear, false, 1.0f,  true,  true,  3, 11, 6,  0.8f, 0.7f },
        { "log, bilinear 0.35",             gt::Encode::LogFromLinear, false, 0.35f, false, true,  0, 2,  13, 1.0f, 1.0f },
        { "log, bilinear 2.2, flipX",       gt::Encode::LogFromLinear, false, 2.2f,  true,  false, 1, 9,  4,  1.7f, 0.2f },
        { "display, preview, exact",        gt::Encode::Display,       true,  1.0f,  false, false, 2, 6,  6,  1.0f, 1.0f },
        { "display, preview, bilinear",     gt::Encode::Display,       true,  0.9f,  true,  false, 3, 1,  8,  0.9f, 0.6f },
        { "log, preview, exact",            gt::Encode::LogFromLinear, true,  1.0f,  false, true,  0, 12, 2,  1.0f, 1.0f },
        { "log, preview, bilinear",         gt::Encode::LogFromLinear, true,  1.25f, true,  true,  1, 4,  10, 1.1f, 0.3f },
        { "amount 0 (exact passthrough)",   gt::Encode::Display,       false, 1.0f,  false, false, 0, 0,  0,  0.0f, 1.0f },
        { "trim 0 (flat sigma)",            gt::Encode::LogFromLinear, false, 1.0f,  false, false, 1, 3,  3,  1.0f, 0.0f },
    };

    std::printf("gt_stamp — %d configurations\n", (int)(sizeof(cases) / sizeof(cases[0])));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        check(cases[i].name, runCase(cases[i], bank, resp, W, H));

    // gt_copy: the GPU passthrough. On the CPU "change nothing" is a no-op on
    // an in-place buffer; here dst is a different allocation, so every pixel
    // still has to move.
    std::printf("\ngt_copy\n");
    {
        const int pitchS = W * 4 + 7, pitchD = W * 4 + 3;   // different pitches
        vector<float> img((size_t)W * H * 4);
        fillARGB(img, W, H, 0x1234u);
        vector<float> src((size_t)pitchS * H, -1.f);
        vector<float> dst((size_t)pitchD * H, -1.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W * 4; ++x)
                src[(size_t)y * pitchS + x] = img[(size_t)y * W * 4 + x];

        ArgsCopy c{};
        c.gCpSrc = src.data();
        c.gCpDst = dst.data();
        c.gCpW = W; c.gCpH = H;
        c.gCpSrcPitch = pitchS; c.gCpDstPitch = pitchD;
        GT_RunGrid(c, W, H);

        float m = 0.f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W * 4; ++x) {
                const float d = std::fabs(dst[(size_t)y * pitchD + x]
                                          - img[(size_t)y * W * 4 + x]);
                if (d > m) m = d;
            }
        check("copy through mismatched pitches", m);
    }

    // The response block is the one piece of state that travels as loose
    // floats. Prove the pack/load round trip independently of the stamp, so a
    // failure there is not mistaken for a math failure.
    std::printf("\nresponse block round trip\n");
    {
        float packed[GT_RESP_FLOATS];
        gt::PackResponse(resp, packed);
        ArgsStamp a{};
        GT_StoreResponse(a, packed);
        GT_Resp got;
        {
            // Mirror GT_RESP_LOAD against the struct the host filled.
            const float *p = &a.gRMid0;
            for (int i = 0; i < GT_RESP_BANDS; ++i)     got.mid[i]  = p[i];
            for (int i = 0; i < 3 * GT_RESP_BANDS; ++i) got.sig[i]  = p[GT_RESP_BANDS + i];
            for (int i = 0; i < 3; ++i)                 got.flat[i] = p[4 * GT_RESP_BANDS + i];
        }
        const GT_Resp ref = gt::CoreResp(resp);
        float m = 0.f;
        for (int i = 0; i < GT_RESP_BANDS; ++i)     m = std::fmax(m, std::fabs(got.mid[i]  - ref.mid[i]));
        for (int i = 0; i < 3 * GT_RESP_BANDS; ++i) m = std::fmax(m, std::fabs(got.sig[i]  - ref.sig[i]));
        for (int i = 0; i < 3; ++i)                 m = std::fmax(m, std::fabs(got.flat[i] - ref.flat[i]));
        check("PackResponse -> Args -> GT_Resp", m);
    }

    std::printf("\n%s (%d/%d passed)\n",
                g_fail ? "=== KERNEL PARITY FAILED ===" : "=== KERNEL PARITY PASSED ===",
                g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
