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

    // ── clean band (<= 0.5) — helper returns current unchanged ───────────
    reset = true;
    expectEq(abrLossDecreaseAction(0.3, 1000, reset), 1000, "loss 0.3 -> clean, unchanged");
    expectTrue(!reset, "loss 0.3 -> no reset");
    expectEq(abrLossDecreaseAction(0.5, 1000, reset), 1000, "loss 0.5 (edge) -> clean");

    // ── mild band (0.5 .. 10) — current - 200, floor MIN ────────────────
    expectEq(abrLossDecreaseAction(0.6, 1000, reset), 800, "loss 0.6 -> 1000-200");
    expectTrue(!reset, "loss 0.6 -> no reset");
    expectEq(abrLossDecreaseAction(5.0, 1000, reset), 800, "loss 5 -> 1000-200");
    expectEq(abrLossDecreaseAction(10.0, 1000, reset), 800, "loss 10 (edge) -> 1000-200");
    // MIN floor: current <= step -> MIN.
    expectEq(abrLossDecreaseAction(5.0, 200, reset), 30, "loss 5, current 200 -> MIN floor");
    expectEq(abrLossDecreaseAction(5.0, 150, reset), 30, "loss 5, current 150 -> MIN floor");
    // MIN floor with a nonzero-but-sub-floor remainder (0 < next < MIN).
    expectEq(abrLossDecreaseAction(5.0, 220, reset), 30, "loss 5, current 220 -> 20 floored to MIN");

    // ── halve band (10 .. 25) — current * 0.50, floor MIN ───────────────
    expectEq(abrLossDecreaseAction(10.1, 1000, reset), 500, "loss 10.1 -> 1000*0.50");
    expectTrue(!reset, "loss 10.1 -> no reset");
    expectEq(abrLossDecreaseAction(18.0, 1000, reset), 500, "loss 18 -> 1000*0.50");
    expectEq(abrLossDecreaseAction(25.0, 1000, reset), 500, "loss 25 (edge) -> 1000*0.50");
    expectEq(abrLossDecreaseAction(18.0, 50, reset), 30, "loss 18, current 50 -> MIN floor");

    // ── MIN band (25 .. 45) — MIN, no reset ─────────────────────────────
    expectEq(abrLossDecreaseAction(25.1, 1000, reset), 30, "loss 25.1 -> MIN");
    expectTrue(!reset, "loss 25.1 -> no reset");
    expectEq(abrLossDecreaseAction(45.0, 1000, reset), 30, "loss 45 (edge) -> MIN, no reset");
    expectTrue(!reset, "loss 45 -> no reset");

    // ── reset band (> 45) — MIN + isReset ───────────────────────────────
    reset = false;
    expectEq(abrLossDecreaseAction(45.1, 1000, reset), 30, "loss 45.1 -> MIN");
    expectTrue(reset, "loss 45.1 -> reset set");
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
