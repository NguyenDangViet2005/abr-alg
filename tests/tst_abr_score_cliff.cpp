// Unit test for the ABR score-cliff predicate, ABRScoreCliff.h (Phase 12).
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure — same style as tests/tst_abr_loss_band.cpp. The
// helper is pure C++ (one free function), so no Qt types are needed.
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_score_cliff.cpp -o tst_abr_score_cliff
//
// The project rule is that the user builds; this file only has to be correct.

#include "../ABRScoreCliff.h"
#include <cstdio>

static int g_failures = 0;

static void expectTrue(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    } else {
        std::printf("ok   %s\n", what);
    }
}

int main()
{
    // The predicate is a pure threshold on (baseline - score). The default from
    // ABRTuning.h is ABR_SCORE_CLIFF_DELTA; we pass it explicitly here so the
    // test pins the MATH, not the current knob value.
    const double D = static_cast<double>(ABR_SCORE_CLIFF_DELTA);

    // ── clear cliffs ──────────────────────────────────────────────────────
    expectTrue( abrScoreCliff(90.0, 30.0, D), "90->30 is a cliff (drop 60 > 50)");
    expectTrue( abrScoreCliff(100.0, 0.0, D), "100->0 is a cliff (full collapse)");

    // ── the >= delta boundary (INCLUSIVE) ─────────────────────────────────
    expectTrue( abrScoreCliff(90.0, 40.0, D), "90->40 is a cliff (drop exactly 50 = delta)");
    expectTrue( abrScoreCliff(50.0, 0.0, D),  "50->0 is a cliff (drop exactly 50 = delta)");
    expectTrue(!abrScoreCliff(90.0, 41.0, D), "90->41 is NOT a cliff (drop 49 < 50)");

    // ── no drop / small noise / upward ────────────────────────────────────
    expectTrue(!abrScoreCliff(90.0, 90.0, D), "flat 90->90 is not a cliff");
    expectTrue(!abrScoreCliff(90.0, 95.0, D), "rise 90->95 is not a cliff");
    expectTrue(!abrScoreCliff(0.0, 0.0, D),   "0->0 is not a cliff");
    expectTrue(!abrScoreCliff(40.0, 40.0, D), "low-but-flat 40->40 is not a cliff");

    // A drop that lands ABOVE the peak-but-by-less-than-delta stays quiet — the
    // whole point of measuring against the RECENT PEAK, not absolute score.
    expectTrue(!abrScoreCliff(80.0, 35.0, D), "80->35 is not a cliff (drop 45 < 50)");

    std::printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
