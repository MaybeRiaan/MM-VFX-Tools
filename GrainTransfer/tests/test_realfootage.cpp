/*  test_realfootage.cpp — real-footage smoke test for Grain Transfer.

    Runs the full analyze→bake→stamp chain on real EXRs from the shared
    TEST FOOTAGE folder (skipped gracefully if absent — the hard gates live
    in test_core.cpp and are footage-independent). Asserts only structural
    health: analysis succeeds and everything stays finite. Character
    judgements are human — PNG dumps land in %GT_TEST_OUT% for review:
      rf_<name>_residual.png   source residual crop (gain-boosted)
      rf_<name>_tile.png       synthesized RPN tile (gain-boosted)
      rf_<name>_crop_in/out.png  512² footage crop before/after stamping
*/

#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#include "tinyexr.h"

#include "GT_Colour.h"
#include "GT_Grain.h"
#include "GT_Response.h"
#include "GT_Stamp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gt;

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { std::printf("  PASS: " __VA_ARGS__); std::printf("\n"); } \
    else { ++g_failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static std::string OutDir()
{
    const char *d = std::getenv("GT_TEST_OUT");
    return d ? std::string(d) : std::string(".");
}

static void DumpGrey(const std::string &name, const float *p, int W, int H, float gain)
{
    std::vector<unsigned char> img((size_t)W * H);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float v = 0.5f + p[i] * gain;
        if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
        img[i] = (unsigned char)(v * 255.f + 0.5f);
    }
    stbi_write_png((OutDir() + "/" + name).c_str(), W, H, 1, img.data(), W);
}

// Crop of ARGB float data → sRGB-ish PNG. display=false applies a 1/2.4
// gamma so linear footage is viewable.
static void DumpCropRGB(const std::string &name, const float *argb, int W, int /*H*/,
                        int x0, int y0, int cw, int ch, bool displayEncoded)
{
    std::vector<unsigned char> img((size_t)cw * ch * 3);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const float *px = argb + ((size_t)(y0 + y) * W + (x0 + x)) * 4;
            for (int c = 0; c < 3; ++c) {
                float v = px[1 + c];
                if (!displayEncoded) v = std::pow(v < 0.f ? 0.f : v, 1.f / 2.4f);
                if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
                img[((size_t)y * cw + x) * 3 + c] = (unsigned char)(v * 255.f + 0.5f);
            }
        }
    stbi_write_png((OutDir() + "/" + name).c_str(), cw, ch, 3, img.data(), cw * 3);
}

struct Candidate {
    const char *path;
    const char *name;
    bool linear;   // true → encode to log before analysis (scene-linear EXR)
};

int main()
{
    const Candidate files[] = {
        { "E:\\OFX DEV\\TEST FOOTAGE\\TEST IMAGERY\\Testing_Imagery-main\\Matas_Alexa_Mini_sample_BT709.exr",
          "alexa709", false },
        { "E:\\OFX DEV\\TEST FOOTAGE\\TEST IMAGERY\\Testing_Imagery-main\\DSC_0454_BT709_linear.exr",
          "dsc_linear", true },
    };

    std::printf("=== Grain Transfer — real-footage smoke test ===\n");

    for (const Candidate &f : files) {
        std::printf("\n[%s] %s\n", f.name, f.path);

        float *rgba = nullptr;
        int W = 0, H = 0;
        const char *err = nullptr;
        {
            FILE *fp = std::fopen(f.path, "rb");
            if (!fp) { std::printf("  skip: file not found\n"); continue; }
            std::fclose(fp);
        }
        if (LoadEXR(&rgba, &W, &H, f.path, &err) != TINYEXR_SUCCESS) {
            std::printf("  skip: EXR load failed (%s)\n", err ? err : "?");
            if (err) FreeEXRErrorMessage(err);
            continue;
        }
        std::printf("  loaded %dx%d\n", W, H);

        const Encode enc = f.linear ? Encode::LogFromLinear : Encode::Display;

        // Apply-space planes.
        std::vector<float> plane[3];
        const float *planes[3];
        for (int c = 0; c < 3; ++c) {
            plane[c].resize((size_t)W * H);
            for (size_t i = 0; i < (size_t)W * H; ++i)
                plane[c][i] = EncodeChannel(rgba[i * 4 + c], enc);
            planes[c] = plane[c].data();
        }

        const int TILE = 256;
        const float blurSigma = 2.0f;

        GrainSpectrum spec;
        const bool ok = MeasureGrainSpectrum(planes, W, H, TILE, blurSigma, spec);
        CHECK(ok && spec.valid(), "analysis succeeded (%d patches)", spec.patchesUsed);
        if (!ok) { free(rgba); continue; }
        std::printf("  info: residual sigma R %.5f G %.5f B %.5f (apply space)\n",
                    spec.sigma[0], spec.sigma[1], spec.sigma[2]);

        std::vector<float> res[3];
        const float *resP[3];
        for (int c = 0; c < 3; ++c) {
            res[c].resize((size_t)W * H);
            HighPassResidual(planes[c], res[c].data(), W, H, blurSigma);
            resP[c] = res[c].data();
        }
        ResponseCurve rc;
        MeasureResponse(planes, resP, W, H, rc);
        CHECK(rc.valid, "response curve measured");
        for (int b = 0; b < ResponseCurve::kBands; ++b)
            std::printf("  info: band %d L=%.3f sigma G %.5f\n",
                        b, rc.lumaMid[b], rc.sigma[1][b]);

        TileBank bank;
        SynthesizeTileBank(spec, 4, 7, bank);
        CHECK(bank.valid(), "tile bank baked");

        int bad = 0;
        for (int t = 0; t < bank.count && !bad; ++t)
            for (int c = 0; c < 3; ++c)
                for (size_t i = 0; i < (size_t)TILE * TILE; ++i)
                    if (!std::isfinite(bank.tile(t, c)[i])) { ++bad; break; }
        CHECK(bad == 0, "tiles finite");

        // Stamp onto the footage itself (worst-case self-application).
        std::vector<float> img((size_t)W * H * 4);
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            img[i * 4 + 0] = 1.f;
            for (int c = 0; c < 3; ++c) img[i * 4 + 1 + c] = rgba[i * 4 + c];
        }
        const int cx = W > 640 ? (W - 512) / 2 : 0;
        const int cy = H > 640 ? (H - 512) / 2 : 0;
        const int cw = W < 512 ? W : 512, chh = H < 512 ? H : 512;
        DumpCropRGB(std::string("rf_") + f.name + "_crop_in.png",
                    img.data(), W, H, cx, cy, cw, chh, !f.linear);

        StampParams sp = MakeFrameStamp(0, 0, bank.count, bank.n);
        sp.amount = 1.f; sp.trim = 1.f; sp.encode = enc;
        StampRows(img.data(), W, H, 0, H, bank, rc, sp);

        int nan = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; ++i)
            if (!std::isfinite(img[i])) ++nan;
        CHECK(nan == 0, "stamped frame finite (%d NaN/Inf)", nan);

        DumpCropRGB(std::string("rf_") + f.name + "_crop_out.png",
                    img.data(), W, H, cx, cy, cw, chh, !f.linear);
        {
            std::vector<float> crop((size_t)cw * chh);
            for (int y = 0; y < chh; ++y)
                for (int x = 0; x < cw; ++x)
                    crop[(size_t)y * cw + x] = res[1][(size_t)(cy + y) * W + (cx + x)];
            DumpGrey(std::string("rf_") + f.name + "_residual.png",
                     crop.data(), cw, chh, 8.f);
        }
        DumpGrey(std::string("rf_") + f.name + "_tile.png",
                 bank.tile(0, 1), TILE, TILE, 8.f);

        free(rgba);
    }

    std::printf("\n=== %s (%d failure%s) ===\n",
                g_failures ? "FAILED" : "REAL-FOOTAGE SMOKE PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
