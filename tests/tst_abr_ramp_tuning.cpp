// Unit test for the ABR ramp tuning pure math: ABRJitter.h + ABRRampStep.h.
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure, so it can be compiled and run alone — same style as
// tests/tst_abr_loss_band.cpp / tst_abr_switch_adopt.cpp. No Qt types needed
// (both helpers are pure C++; ABRJitter is templated so it takes a plain array).
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_ramp_tuning.cpp -o tst_abr_ramp_tuning
//
// The project rule is that the user builds; this file only has to be correct.

#include "../ABRJitter.h"
#include "../ABRRampStep.h"
#include <cstdio>

static int g_failures = 0;

static void expectEqU(unsigned int got, unsigned int want, const char *what)
{
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s: got %u, want %u\n", what, got, want);
    } else {
        std::printf("ok   %s = %u\n", what, got);
    }
}

static void expectTrue(bool ok, const char *what)
{
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", what); }
    else     { std::printf("ok   %s\n", what); }
}

static void expectNear(double got, double want, double tol, const char *what)
{
    if (got < want - tol || got > want + tol) {
        ++g_failures;
        std::printf("FAIL %s: got %.4f, want %.4f (+-%.4f)\n", what, got, want, tol);
    } else {
        std::printf("ok   %s = %.4f\n", what, got);
    }
}

int main()
{
    // ── abrJitterStddev ─────────────────────────────────────────────────
    // Degenerate windows: a single sample (or none) has no spread -> 0.0,
    // matching the old calculateJitter() early-out.
    {
        const unsigned int one[] = { 100 };
        expectNear(abrJitterStddev(one, 1), 0.0, 1e-9, "jitter: 1 sample -> 0");
        expectNear(abrJitterStddev(one, 0), 0.0, 1e-9, "jitter: 0 samples -> 0");
    }

    // Clean window of 5 (mild wobble): population stddev ~= 5.1 ms, i.e. BELOW
    // the 15 ms stable threshold -> the ramp gate OPENS.
    {
        const unsigned int clean5[] = { 50, 60, 55, 45, 55 };
        const double j = abrJitterStddev(clean5, 5);
        expectNear(j, 5.099, 0.01, "jitter: clean window-5 ~= 5.1ms");
        expectTrue(j < static_cast<double>(ABR_JITTER_STABLE_THRESHOLD_MS),
                   "jitter: clean window-5 < 15ms (gate opens)");
    }

    // Congested window of 5 (3 high samples in a row, as seen in qos-5.log):
    // stddev ~= 125 ms, well ABOVE 15 -> the ramp gate BLOCKS.
    {
        const unsigned int spike5[] = { 55, 300, 310, 320, 55 };
        const double j = abrJitterStddev(spike5, 5);
        expectNear(j, 125.12, 1.0, "jitter: 3-sample spike window-5 ~= 125ms");
        expectTrue(j >= static_cast<double>(ABR_JITTER_STABLE_THRESHOLD_MS),
                   "jitter: 3-sample spike window-5 >= 15ms (gate blocks)");
    }

    // THE LEVER: the window only "remembers" a spike for N samples. Same 10
    // RTT samples — a genuine 3-tick spike early in the series — but measured
    // over the NEWER 5 vs the OLDER 5: once the spike has left the window the
    // jitter collapses, so a shorter window reopens the gate sooner.
    {
        // samples[0..9] oldest -> newest; spike sits at indices 0..2.
        const unsigned int series[] = { 320, 310, 300, 55, 55, 55, 55, 55, 54, 56 };
        const double older5 = abrJitterStddev(series, 5);          // {320,310,300,55,55}
        const double newer5 = abrJitterStddev(series + 5, 5);      // {55,55,55,54,56}
        expectTrue(older5 >= static_cast<double>(ABR_JITTER_STABLE_THRESHOLD_MS),
                   "jitter: window still holds spike -> blocks");
        expectTrue(newer5 < static_cast<double>(ABR_JITTER_STABLE_THRESHOLD_MS),
                   "jitter: spike left window-5 -> gate reopens");
    }

    // ── abrRampStepKbps ─────────────────────────────────────────────────
    // qdelay strictly below ABR_QUEUE_DELAY_LOW_THRESHOLD_MS (100) -> FAST.
    expectEqU(abrRampStepKbps(0.0),   ABR_RAMP_STEP_FAST_KBPS, "step: qdelay 0 -> FAST");
    expectEqU(abrRampStepKbps(5.0),   ABR_RAMP_STEP_FAST_KBPS, "step: qdelay 5 -> FAST");
    expectEqU(abrRampStepKbps(99.9),  ABR_RAMP_STEP_FAST_KBPS, "step: qdelay 99.9 -> FAST");
    // Boundary is EXCLUSIVE on FAST: exactly 100 is NOT low -> SLOW.
    expectEqU(abrRampStepKbps(100.0), ABR_RAMP_STEP_SLOW_KBPS, "step: qdelay 100 (edge) -> SLOW");
    // Higher (still below the HIGH threshold) -> SLOW.
    expectEqU(abrRampStepKbps(150.0), ABR_RAMP_STEP_SLOW_KBPS, "step: qdelay 150 -> SLOW");

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
