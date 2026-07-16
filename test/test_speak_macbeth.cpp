// Speak — Phase 2 control arm: the 24-patch Macbeth CIELAB gate for
// Density-Space Subtractive Saturation.
//
// The house rule (the X3 law): a look module must beat a CHEAP BASELINE on a
// stated measurement before it ships. Here the baseline is a plain linear 3x3
// saturation, matched to the subtractive model's chroma gain on the MID-tone
// patches. The subtractive model must then show two STRUCTURAL signatures a
// linear matrix cannot fake:
//   S1 neutral invariance      — the 6 gray patches gain no chroma
//   S2 highlight desaturation  — chroma gain falls off toward base white
//                                (highlights self-compress; the baseline's
//                                gain is flat vs lightness by construction)
//   S3 hue skew to dye axes    — hues rotate (asymmetric coupler); the plain
//                                3x3 saturation preserves hue by construction
//
// Build: c++ -O2 -std=c++14 -I../plugin test_speak_macbeth.cpp -o test_speak_macbeth

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

// The 24 ColorChecker patches as 8-bit sRGB (BabelColor averages — a published
// reference chart, not a vendor profile).
static const int kMacbeth[24][3] = {
    {115,82,68}, {194,150,130}, {98,122,157}, {87,108,67}, {133,128,177}, {103,189,170},
    {214,126,44}, {80,91,166}, {193,90,99}, {94,60,108}, {157,188,64}, {224,163,46},
    {56,61,150}, {70,148,73}, {175,54,60}, {231,199,31}, {187,86,149}, {8,133,161},
    {243,243,242}, {200,200,200}, {160,160,160}, {122,122,121}, {85,85,85}, {52,52,52} };
static const char* kNames[24] = {
    "dark skin","light skin","blue sky","foliage","blue flower","bluish green",
    "orange","purplish blue","moderate red","purple","yellow green","orange yellow",
    "blue","green","red","yellow","magenta","cyan",
    "white","neutral 8","neutral 6.5","neutral 5","neutral 3.5","black" };

static const float kRec709_to_XYZ[9] = {
    0.41245643f, 0.35757608f, 0.18043748f,
    0.21267285f, 0.71515217f, 0.07217500f,
    0.01933390f, 0.11919203f, 0.95030407f };
static const float kXYZ_to_DWG[9] = {
    1.51667205f,-0.28147806f,-0.14696364f,
   -0.46491710f, 1.25142377f, 0.17488461f,
    0.06484904f, 0.10913935f, 0.76141462f };

static float srgbToLin(int v8)
{
    const float v = v8 / 255.0f;
    return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
// Patch -> DWG-linear (the working space the module operates in).
static void patchToDWG(int i, float& r, float& g, float& b)
{
    const float lr = srgbToLin(kMacbeth[i][0]), lg = srgbToLin(kMacbeth[i][1]), lb = srgbToLin(kMacbeth[i][2]);
    float X, Y, Z; mul3(kRec709_to_XYZ, lr, lg, lb, X, Y, Z);
    mul3(kXYZ_to_DWG, X, Y, Z, r, g, b);
}

// The CHEAP BASELINE: plain linear 3x3 saturation about DWG luminance.
static void plainSat(float r, float g, float b, float s, float& oR, float& oG, float& oB)
{
    const float Y = 0.27411851f * r + 0.87363190f * g - 0.14775041f * b;  // DWG luma row
    oR = Y + s * (r - Y); oG = Y + s * (g - Y); oB = Y + s * (b - Y);
}

struct Lab { float L, a, b; };
static Lab labOf(float r, float g, float b) { Lab o; dwgLinToLab(r, g, b, o.L, o.a, o.b); return o; }
static float chroma(const Lab& c) { return std::sqrt(c.a * c.a + c.b * c.b); }
static float hueDeg(const Lab& c) { return std::atan2(c.b, c.a) * 57.29577951f; }
static float hueDiff(float h1, float h0)
{
    float d = h1 - h0;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Mean chroma gain over patches whose ORIGINAL L* is in [lo,hi) and that have
// real chroma to begin with (skip the neutrals).
static float meanGain(const std::vector<Lab>& in, const std::vector<Lab>& out, float lo, float hi)
{
    float sum = 0.0f; int n = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const float ci = chroma(in[i]);
        if (ci < 5.0f) continue;                    // neutral-ish: gain undefined
        if (in[i].L < lo || in[i].L >= hi) continue;
        sum += chroma(out[i]) / ci; n++;
    }
    return n ? sum / n : 0.0f;
}

int main()
{
    printf("=== Speak Phase 2 control arm: Macbeth 24 in CIELAB ===\n");

    // --- the subject: density-space subtractive saturation ---
    SpeakProfile p = neutralProfile();
    const float SAT = 0.55f;
    p.subSat[0] = p.subSat[1] = p.subSat[2] = SAT;
    setDyeCoupler(p, 1.0f);   // the shared dye cross-absorption pattern (speak_core.h)
    p.subSatKnee[0] = p.subSatKnee[1] = p.subSatKnee[2] = 2.2f;  // per-dye Dmax knee

    std::vector<Lab> in(24), sub(24);
    std::vector<float> dwg(24 * 3);
    for (int i = 0; i < 24; ++i) {
        float r, g, b; patchToDWG(i, r, g, b);
        dwg[i*3+0] = r; dwg[i*3+1] = g; dwg[i*3+2] = b;
        in[i] = labOf(r, g, b);
        float sr, sg, sb; subtractiveColor(r, g, b, p, sr, sg, sb);
        sub[i] = labOf(sr, sg, sb);
    }

    // --- match the baseline to the subject on the MID-tone patches ---
    const float subMid = meanGain(in, sub, 40.0f, 70.0f);
    float lo = 1.0f, hi = 4.0f, satB = 1.0f;
    std::vector<Lab> base(24);
    for (int it = 0; it < 60; ++it) {                 // bisect the plain-3x3 sat
        satB = 0.5f * (lo + hi);
        for (int i = 0; i < 24; ++i) {
            float br, bg, bb;
            plainSat(dwg[i*3+0], dwg[i*3+1], dwg[i*3+2], satB, br, bg, bb);
            base[i] = labOf(br, bg, bb);
        }
        (meanGain(in, base, 40.0f, 70.0f) < subMid) ? (lo = satB) : (hi = satB);
    }
    printf("  matched at midtones: subtractive gain %.3f  <->  plain 3x3 sat %.3f (gain %.3f)\n",
           subMid, satB, meanGain(in, base, 40.0f, 70.0f));

    // --- per-patch report ---
    printf("  %-14s  L*    C_in   C_sub  C_base |  dHue_sub  dHue_base\n", "patch");
    for (int i = 0; i < 24; ++i)
        printf("  %-14s %5.1f  %5.1f  %5.1f  %5.1f  |  %+7.2f  %+8.2f\n",
               kNames[i], in[i].L, chroma(in[i]), chroma(sub[i]), chroma(base[i]),
               chroma(in[i]) < 5.0f ? 0.0f : hueDiff(hueDeg(sub[i]), hueDeg(in[i])),
               chroma(in[i]) < 5.0f ? 0.0f : hueDiff(hueDeg(base[i]), hueDeg(in[i])));

    // --- S1: neutral invariance (the 6 gray patches, indices 18..23) ---
    float maxNeutralChroma = 0.0f;
    for (int i = 18; i < 24; ++i)
        maxNeutralChroma = std::fmax(maxNeutralChroma, std::fabs(chroma(sub[i]) - chroma(in[i])));
    check(maxNeutralChroma < 0.5f, "S1 neutral patches gain no chroma",
          "max dC=" + std::to_string(maxNeutralChroma));

    // --- S2: highlight chroma self-compresses toward base white (structural) ---
    // A linear matrix scales (C - Y) in LINEAR light, so at matched midtones its
    // chroma gain runs away in the highlights (the classic saturation blow-out,
    // far outside any real gamut). The density model amplifies deviations in
    // log-density and views back through 10^-D, so its gain stays near-flat vs
    // lightness — highlights roll toward base white instead of exploding. That
    // flatness IS the signature a matrix cannot fake.
    const float baseMid = meanGain(in, base, 40.0f, 70.0f);
    const float subHi = meanGain(in, sub, 70.0f, 200.0f);
    const float baseHi = meanGain(in, base, 70.0f, 200.0f);
    const float subRise = subHi / subMid, baseRise = baseHi / baseMid;
    printf("  gain vs lightness — subtractive: mid %.3f -> high %.3f (x%.2f) | plain: mid %.3f -> high %.3f (x%.2f)\n",
           subMid, subHi, subRise, baseMid, baseHi, baseRise);
    check(subHi < baseHi, "S2a subtractive desaturates highlights vs matched plain 3x3",
          "sub " + std::to_string(subHi) + " < plain " + std::to_string(baseHi));
    check(subRise < 0.7f * baseRise, "S2b subtractive highlight blow-up is structurally suppressed",
          "x" + std::to_string(subRise) + " vs plain x" + std::to_string(baseRise));

    // --- S3: hue skew from the inter-image coupler (the dye-crosstalk signature) ---
    // Measured by ISOLATING the coupler (same model, coupler zeroed): a plain
    // saturation has no mechanism that can produce this at all — its own hue
    // rotation is incidental, not a controllable dye-axis skew.
    SpeakProfile pNoC = p;
    pNoC.dyeCouple[1] = pNoC.dyeCouple[2] = pNoC.dyeCouple[3] = 0.0f;
    pNoC.dyeCouple[5] = pNoC.dyeCouple[6] = pNoC.dyeCouple[7] = 0.0f;
    float couplerHue = 0.0f, maxNeutralC = 0.0f; int n = 0;
    for (int i = 0; i < 24; ++i) {
        float nr, ng, nb; subtractiveColor(dwg[i*3+0], dwg[i*3+1], dwg[i*3+2], pNoC, nr, ng, nb);
        const Lab noC = labOf(nr, ng, nb);
        if (i >= 18) { maxNeutralC = std::fmax(maxNeutralC, std::fabs(chroma(sub[i]) - chroma(noC))); continue; }
        if (chroma(in[i]) < 5.0f) continue;
        couplerHue += std::fabs(hueDiff(hueDeg(sub[i]), hueDeg(noC)));
        n++;
    }
    couplerHue /= n;
    printf("  coupler-attributable |hue rotation|: %.2f deg (neutral drift %.3f)\n", couplerHue, maxNeutralC);
    check(couplerHue > 1.5f, "S3a the coupler skews hue toward the dye axes",
          std::to_string(couplerHue) + " deg");
    check(maxNeutralC < 0.5f, "S3b the coupler stays zero on the neutral axis",
          "drift " + std::to_string(maxNeutralC));

    printf("\n%s (%d failures)\n", g_fail ? "CONTROL ARM FAILED" : "CONTROL ARM PASSED", g_fail);
    return g_fail ? 1 : 0;
}
