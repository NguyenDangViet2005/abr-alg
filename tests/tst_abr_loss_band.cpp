// Unit test for the ABR loss-ladder band math, ABRLossBand.h (Phase 11).
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure, so it can be compiled and run alone — same style as
// tests/tst_qs_encoder.cpp. No Qt types are needed (the helper is pure C++).
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_loss_band.cpp -o tst_abr_loss_band
//
// The project rule is that the user builds; this file only has to be correct.

#include "../ABRLossBand.h"
#include <cstdio>

static int g_failures = 0;

static void expectEq(unsigned int got, unsigned int want, const char *what)
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

int main()
{
    bool reset = false;

    // ── clean band (<= 0.8) — helper returns current unchanged ───────────
    reset = true;
    expectEq(abrLossDecreaseAction(0.5, 1000, reset), 1000, "loss 0.5 -> clean, unchanged");
    expectTrue(!reset, "loss 0.5 -> no reset");
    expectEq(abrLossDecreaseAction(0.8, 1000, reset), 1000, "loss 0.8 (edge) -> clean");

    // ── mild band (0.8 .. 20) — current - 200, floor MIN ────────────────
    expectEq(abrLossDecreaseAction(0.9, 1000, reset), 800, "loss 0.9 -> 1000-200");
    expectTrue(!reset, "loss 0.9 -> no reset");
    expectEq(abrLossDecreaseAction(10.0, 1000, reset), 800, "loss 10 -> 1000-200");
    expectEq(abrLossDecreaseAction(20.0, 1000, reset), 800, "loss 20 (edge) -> 1000-200");
    // MIN floor: current <= step -> MIN.
    expectEq(abrLossDecreaseAction(10.0, 200, reset), 30, "loss 10, current 200 -> MIN floor");
    expectEq(abrLossDecreaseAction(10.0, 150, reset), 30, "loss 10, current 150 -> MIN floor");
    // MIN floor with a nonzero-but-sub-floor remainder (0 < next < MIN).
    expectEq(abrLossDecreaseAction(10.0, 220, reset), 30, "loss 10, current 220 -> 20 floored to MIN");

    // ── halve band (20 .. 50) — current * 0.50, floor MIN ───────────────
    expectEq(abrLossDecreaseAction(20.1, 1000, reset), 500, "loss 20.1 -> 1000*0.50");
    expectTrue(!reset, "loss 20.1 -> no reset");
    expectEq(abrLossDecreaseAction(35.0, 1000, reset), 500, "loss 35 -> 1000*0.50");
    expectEq(abrLossDecreaseAction(50.0, 1000, reset), 500, "loss 50 (edge) -> 1000*0.50");
    expectEq(abrLossDecreaseAction(35.0, 50, reset), 30, "loss 35, current 50 -> MIN floor");

    // ── MIN band (50 .. 70) — MIN, no reset ─────────────────────────────
    expectEq(abrLossDecreaseAction(50.1, 1000, reset), 30, "loss 50.1 -> MIN");
    expectTrue(!reset, "loss 50.1 -> no reset");
    expectEq(abrLossDecreaseAction(70.0, 1000, reset), 30, "loss 70 (edge) -> MIN, no reset");
    expectTrue(!reset, "loss 70 -> no reset");

    // ── reset band (> 70) — MIN + isReset ───────────────────────────────
    reset = false;
    expectEq(abrLossDecreaseAction(70.1, 1000, reset), 30, "loss 70.1 -> MIN");
    expectTrue(reset, "loss 70.1 -> reset set");
    reset = false;
    expectEq(abrLossDecreaseAction(95.0, 1000, reset), 30, "loss 95 -> MIN");
    expectTrue(reset, "loss 95 -> reset set");

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
