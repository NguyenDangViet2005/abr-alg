// Unit test for the ABR switch-seed decision, ABRSwitchSeed.h (Phase 13).
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure — same style as tests/tst_abr_score_cliff.cpp and
// tests/tst_abr_loss_band.cpp. Both helpers are pure C++ (two free functions),
// so no Qt types are needed.
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_switch_seed.cpp -o tst_abr_switch_seed

#include "../ABRSwitchSeed.h"
#include <cstdio>

static int g_failures = 0;

static void expectEq(unsigned int got, unsigned int want, const char *what)
{
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s: got %u, want %u\n", what, got, want);
    } else {
        std::printf("ok   %s\n", what);
    }
}

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
    const unsigned int MIN  = static_cast<unsigned int>(ABR_MIN_BITRATE_KBPS);   // 30
    const unsigned int HEAL = static_cast<unsigned int>(ABR_SWITCH_SEED_HEALTHY_KBPS); // 500
    const unsigned int FLO  = static_cast<unsigned int>(ABR_SWITCH_SEED_FLOOR_KBPS);   // 100
    const double GOOD_Q = static_cast<double>(ABR_QUEUE_DELAY_LOW_THRESHOLD_MS);  // 100
    const double MIN_SCORE = static_cast<double>(ABR_OVERSHOOT_MIN_SCORE_AT_STEPUP); // 70

    // ── health predicate ──────────────────────────────────────────────────
    expectTrue( abrSwitchLinkHealthy(true, false, 95.0, 23.0),
                "usable, no loss, score 95, q 23 -> healthy (the eth1-after-recovery case)");
    expectTrue(!abrSwitchLinkHealthy(false, false, 95.0, 23.0),
                "not usable -> NOT healthy");
    expectTrue(!abrSwitchLinkHealthy(true, true, 95.0, 23.0),
                "loss this tick -> NOT healthy");
    expectTrue(!abrSwitchLinkHealthy(true, false, 69.9, 23.0),
                "score below bar -> NOT healthy");
    expectTrue( abrSwitchLinkHealthy(true, false, MIN_SCORE, 23.0),
                "score exactly at bar -> healthy (inclusive)");
    expectTrue(!abrSwitchLinkHealthy(true, false, 95.0, GOOD_Q),
                "qdelay exactly at threshold -> NOT healthy (strict <)");

    // ── seed value, healthy ───────────────────────────────────────────────
    // The bug case: pinned at 30, link recovered -> jump to the bootstrap.
    expectEq( abrSwitchSeedKbps(MIN, 2000u, true), HEAL,
              "floor + healthy -> healthy bootstrap");
    // Low-ceiling link: cap below the bootstrap -> clamped to cap.
    expectEq( abrSwitchSeedKbps(MIN, 400u, true), 400u,
              "floor + healthy -> clamped to a low cap");
    // Cap exactly at the bootstrap -> the bootstrap.
    expectEq( abrSwitchSeedKbps(MIN, HEAL, true), HEAL,
              "floor + healthy, cap == bootstrap -> bootstrap");

    // ── seed value, not healthy ───────────────────────────────────────────
    expectEq( abrSwitchSeedKbps(MIN, 2000u, false), FLO,
              "floor + unhealthy -> one-step lift (not the bootstrap)");
    // Cap below the one-step lift -> clamped to cap (still above floor here).
    expectEq( abrSwitchSeedKbps(MIN, 80u, false), 80u,
              "floor + unhealthy -> clamped to a mid cap");
    // A pathological cap at/under the floor: must not dip below MIN.
    expectEq( abrSwitchSeedKbps(MIN, 10u, false), MIN,
              "floor + cap below floor -> floored at MIN");
    expectEq( abrSwitchSeedKbps(MIN, 10u, true), MIN,
              "floor + healthy but cap below floor -> floored at MIN");

    // ── not at the floor: untouched, regardless of health ─────────────────
    expectEq( abrSwitchSeedKbps(600u, 2000u, true), 600u,
              "above floor + healthy -> unchanged (ramp owns it)");
    expectEq( abrSwitchSeedKbps(600u, 2000u, false), 600u,
              "above floor + unhealthy -> unchanged (only the floor is seeded)");
    expectEq( abrSwitchSeedKbps(MIN + 1u, 2000u, true), MIN + 1u,
              "one above floor -> unchanged (boundary: >MIN)");
    // Exactly at the floor is seeded (boundary: <=MIN).
    expectEq( abrSwitchSeedKbps(MIN, 2000u, true), HEAL,
              "exactly at floor -> seeded (boundary)");

    if (g_failures == 0) {
        std::printf("\nPASS: all switch-seed checks passed\n");
        return 0;
    }
    std::printf("\nFAIL: %d check(s) failed\n", g_failures);
    return 1;
}
