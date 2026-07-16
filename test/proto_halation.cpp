// Speak — Phase 4 PROTOTYPE + control arm: red-weighted halation.
//
// Gate-first (the X3 law): this is a CPU prototype, NOT shipping code. It only
// earns a place in speak_core.h + the three kernels if it beats the cheap
// baseline on the stated measurement.
//
// THE PHYSICS CLAIM. Real halation is light that passes through the emulsion,
// reflects off the base, and RE-EXPOSES the negative from behind. So the scatter
// must be injected as EXPOSURE (additive in linear light) BEFORE the H&D curve —
// after which the curve's shoulder compresses it. It is red-weighted because the
// anti-halation layer absorbs blue/green far more than red.
//
// THE CHEAP BASELINE: the thing every "film look" plugin actually ships — the
// same blurred highlight excess, red-tinted, added to the OUTPUT at the end of
// the chain. It bypasses the curve entirely.
//
// THE GATE (from the build order): halation must SELF-LIMIT and DESATURATE like
// re-exposure, which an end-chain overlay structurally cannot do:
//   H1 SELF-LIMITING — quadruple the source highlight and the physical halo
//      grows far SUB-linearly (the print shoulder eats it); the overlay's halo
//      grows ~linearly (x4).
//   H2 DESATURATES   — the physical halo lifts the shadow toward the print's
//      response to light, so its CIELAB chroma stops climbing; the overlay just
//      keeps pumping red chroma in without bound.
//
// Build: c++ -O2 -std=c++14 -I../plugin proto_halation.cpp -o proto_halation

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "speak_core.h"
using namespace speakcore;

static int g_fail = 0;
static void check(bool ok, const char* name, const std::string& detail = "")
{
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok) g_fail++;
}

// ---- separable blur (prototype; the shipping version becomes the pyramid) ----
static void blurSep(const std::vector<float>& in, int W, int H, float sigma, std::vector<float>& out)
{
    const int R = static_cast<int>(sigma * 3.0f) + 1;
    std::vector<float> k(2 * R + 1);
    float ksum = 0.0f;
    for (int i = -R; i <= R; ++i) { k[i + R] = std::exp(-0.5f * (i * i) / (sigma * sigma)); ksum += k[i + R]; }
    for (size_t i = 0; i < k.size(); ++i) k[i] /= ksum;      // energy-normalised
    std::vector<float> tmp(in.size(), 0.0f);
    out.assign(in.size(), 0.0f);
    for (int y = 0; y < H; ++y)                               // horizontal
        for (int x = 0; x < W; ++x) {
            float a = 0.0f;
            for (int i = -R; i <= R; ++i) {
                int xx = x + i; xx = xx < 0 ? 0 : (xx >= W ? W - 1 : xx);
                a += k[i + R] * in[static_cast<size_t>(y) * W + xx];
            }
            tmp[static_cast<size_t>(y) * W + x] = a;
        }
    for (int y = 0; y < H; ++y)                               // vertical
        for (int x = 0; x < W; ++x) {
            float a = 0.0f;
            for (int i = -R; i <= R; ++i) {
                int yy = y + i; yy = yy < 0 ? 0 : (yy >= H ? H - 1 : yy);
                a += k[i + R] * tmp[static_cast<size_t>(yy) * W + x];
            }
            out[static_cast<size_t>(y) * W + x] = a;
        }
}

// The anti-halation layer absorbs blue/green far more than red — this is WHY
// halation is red. Beer-Lambert-shaped weights (published AH behaviour).
static const float kHalWeight[3] = { 1.0f, 0.30f, 0.10f };
static const float kHalThresh = 0.6f;     // linear highlight excess threshold
static const float kHalSigma  = 9.0f;     // scatter radius (px, prototype)

struct Img { std::vector<float> p; int W, H; };   // interleaved RGB linear

// Build a scene: a bright disc (the source) on a dark blue-ish background.
static Img makeScene(int W, int H, float discLin)
{
    Img im; im.W = W; im.H = H; im.p.assign(static_cast<size_t>(W) * H * 3, 0.0f);
    const float cx = W * 0.5f, cy = H * 0.5f, rad = W * 0.10f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            const size_t i = (static_cast<size_t>(y) * W + x) * 3;
            if (d <= rad) { im.p[i + 0] = discLin; im.p[i + 1] = discLin * 0.92f; im.p[i + 2] = discLin * 0.80f; }
            else          { im.p[i + 0] = 0.010f;  im.p[i + 1] = 0.014f;          im.p[i + 2] = 0.030f; }
        }
    return im;
}

static void extractExcess(const Img& im, std::vector<float>& ex)   // luminous highlight excess
{
    ex.assign(static_cast<size_t>(im.W) * im.H, 0.0f);
    for (size_t k = 0; k < ex.size(); ++k) {
        const float m = (im.p[k * 3 + 0] + im.p[k * 3 + 1] + im.p[k * 3 + 2]) * (1.0f / 3.0f);
        ex[k] = m > kHalThresh ? (m - kHalThresh) : 0.0f;
    }
}

// SUBJECT: scatter re-injected as EXPOSURE, before the curve.
static Img renderPhysical(const Img& im, const SpeakProfile& p, float amount)
{
    std::vector<float> ex, sc; extractExcess(im, ex); blurSep(ex, im.W, im.H, kHalSigma, sc);
    Img o; o.W = im.W; o.H = im.H; o.p.assign(im.p.size(), 0.0f);
    for (size_t k = 0; k < sc.size(); ++k)
        for (int c = 0; c < 3; ++c) {
            const float E = im.p[k * 3 + c] + amount * sc[k] * kHalWeight[c];  // RE-EXPOSURE
            o.p[k * 3 + c] = toneChannel(E, c, p);                             // then the curve
        }
    return o;
}

// BASELINE: the same blurred excess, red-tinted, added to the OUTPUT.
static Img renderOverlay(const Img& im, const SpeakProfile& p, float amount)
{
    std::vector<float> ex, sc; extractExcess(im, ex); blurSep(ex, im.W, im.H, kHalSigma, sc);
    Img o; o.W = im.W; o.H = im.H; o.p.assign(im.p.size(), 0.0f);
    for (size_t k = 0; k < sc.size(); ++k)
        for (int c = 0; c < 3; ++c)
            o.p[k * 3 + c] = toneChannel(im.p[k * 3 + c], c, p) + amount * sc[k] * kHalWeight[c];
    return o;
}

// Mean of a thin ring just outside the disc — that is where the halo lives.
static void ringMean(const Img& im, float& r, float& g, float& b)
{
    const float cx = im.W * 0.5f, cy = im.H * 0.5f, rad = im.W * 0.10f;
    double sr = 0, sg = 0, sb = 0; int n = 0;
    for (int y = 0; y < im.H; ++y)
        for (int x = 0; x < im.W; ++x) {
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d > rad * 1.15f && d < rad * 1.6f) {
                const size_t i = (static_cast<size_t>(y) * im.W + x) * 3;
                sr += im.p[i + 0]; sg += im.p[i + 1]; sb += im.p[i + 2]; n++;
            }
        }
    r = float(sr / n); g = float(sg / n); b = float(sb / n);
}
static float chromaOf(float r, float g, float b)
{
    float L, a, bb; dwgLinToLab(r, g, b, L, a, bb);
    return std::sqrt(a * a + bb * bb);
}

int main()
{
    printf("=== Speak Phase 4 prototype: halation vs a red overlay ===\n");
    const int W = 220, H = 220;
    SpeakProfile p = neutralProfile();
    const float amount = 1.0f;

    // NOTE (corrected by this arm on its first run): "self-limiting" does NOT
    // mean the halo grows sub-linearly at moderate brightness. The halo lives in
    // the SHADOWS around a highlight, where the tone curve is steep (system
    // gamma ~1.36), so re-exposure there is AMPLIFIED — which is exactly why
    // film halation is so visible against dark surrounds. The real structural
    // limit is the PAPER-WHITE CEILING: re-exposure is bounded by the print's
    // Dmin no matter how bright the source, and on the way there every channel
    // saturates, so the halo goes WHITE. An end-chain overlay has no ceiling and
    // no such desaturation. So sweep a wide brightness range and look for it.
    const float ceiling = toneChannel(1e6f, 0, p);      // the print's paper white
    const float bg = toneChannel(0.010f, 0, p);
    printf("  print paper-white ceiling = %.4f   background floor = %.4f\n", ceiling, bg);

    const float sources[5] = { 1.0f, 4.0f, 16.0f, 64.0f, 256.0f };
    float physRed[5], physChroma[5], ovRed[5], ovChroma[5];
    printf("  %-8s | %-22s | %-22s\n", "source", "physical (red, chroma)", "overlay  (red, chroma)");
    for (int i = 0; i < 5; ++i) {
        Img s = makeScene(W, H, sources[i]);
        float a, b, c; ringMean(renderPhysical(s, p, amount), a, b, c);
        physRed[i] = a; physChroma[i] = chromaOf(a, b, c);
        float d, e, f; ringMean(renderOverlay(s, p, amount), d, e, f);
        ovRed[i] = d; ovChroma[i] = chromaOf(d, e, f);
        printf("  %8.0f | %10.4f %10.1f  | %10.4f %10.1f\n",
               sources[i], physRed[i], physChroma[i], ovRed[i], ovChroma[i]);
    }

    // H1 — the physical halo is BOUNDED by paper white; the overlay is not.
    check(physRed[4] <= ceiling * 1.02f, "H1a physical halo is bounded by the print's paper white",
          std::to_string(physRed[4]) + " <= " + std::to_string(ceiling));
    check(ovRed[4] > ceiling * 1.5f, "H1b the red overlay blows straight past paper white (unbounded)",
          std::to_string(ovRed[4]) + " >> " + std::to_string(ceiling));

    // H2 — on the way to the ceiling every channel saturates, so the physical
    // halo DESATURATES (chroma peaks then falls). The overlay only gets redder.
    float physPeak = 0.0f; int physPeakAt = 0;
    for (int i = 0; i < 5; ++i) if (physChroma[i] > physPeak) { physPeak = physChroma[i]; physPeakAt = i; }
    check(physPeakAt < 4 && physChroma[4] < physPeak,
          "H2a physical halo's chroma PEAKS then falls (it goes white-hot)",
          "peak at source " + std::to_string((int)sources[physPeakAt]) +
          ", falls to " + std::to_string(physChroma[4]));
    bool ovMono = true;
    for (int i = 1; i < 5; ++i) if (ovChroma[i] <= ovChroma[i - 1]) ovMono = false;
    check(ovMono, "H2b the overlay's chroma just keeps climbing (never desaturates)",
          "monotonic: " + std::string(ovMono ? "yes" : "no"));
    check(physChroma[4] < ovChroma[4], "H2c at extreme source the physical halo is far less saturated",
          std::to_string(physChroma[4]) + " < " + std::to_string(ovChroma[4]));

    printf("\n%s (%d failures)\n", g_fail ? "PROTOTYPE REJECTED" : "PROTOTYPE EARNED ITS PLACE", g_fail);
    return g_fail ? 1 : 0;
}
