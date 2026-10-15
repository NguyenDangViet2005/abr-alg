// Unit test for the ABR switch-ADOPT decision, ABRSwitchAdopt.h.
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure — same style as tests/tst_abr_score_cliff.cpp and
// tests/tst_abr_loss_band.cpp. Both helpers are pure C++ (two free functions),
// so no Qt types are needed — ABRSwitchAdopt.h deliberately takes `long long`
// for the debounce clock rather than a Qt type, keeping it standalone-buildable.
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_switch_adopt.cpp -o tst_abr_switch_adopt

#include "../ABRSwitchAdopt.h"
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

static void expectBool(bool got, bool want, const char *what)
{
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s: got %d, want %d\n", what, got, want);
    } else {
        std::printf("ok   %s\n", what);
    }
}

int main()
{
    const unsigned int MIN  = static_cast<unsigned int>(ABR_MIN_BITRATE_KBPS);   // 30
    const unsigned int MAX  = static_cast<unsigned int>(ABR_MAX_BITRATE_KBPS);   // 2000
    const long long DEBOUNCE =
        static_cast<long long>(ABR_SWITCH_DEBOUNCE_MS);                          // 2000

    // ── adopt: in-range carry is untouched ────────────────────────────────
    // The bug case: camera was running 1200, new primary adopts 1200 verbatim.
    expectEq( abrSwitchAdoptKbps(1200u, MIN, MAX), 1200u,
              "in-range carry -> unchanged (the eth2->eth1 fix)");
    expectEq( abrSwitchAdoptKbps(MIN, MIN, MAX), MIN,
              "carry exactly at MIN -> unchanged (boundary)");
    expectEq( abrSwitchAdoptKbps(MAX, MIN, MAX), MAX,
              "carry exactly at MAX -> unchanged (boundary)");

    // ── adopt: below floor clamps up ──────────────────────────────────────
    // A stale carry of 0 (no dispatch ever happened) lifts to the floor.
    expectEq( abrSwitchAdoptKbps(0u, MIN, MAX), MIN,
              "carry 0 (no prior dispatch) -> floored at MIN");
    expectEq( abrSwitchAdoptKbps(MIN - 1u, MIN, MAX), MIN,
              "carry one below MIN -> floored at MIN");

    // ── adopt: above max clamps down ──────────────────────────────────────
    expectEq( abrSwitchAdoptKbps(MAX + 1u, MIN, MAX), MAX,
              "carry above MAX -> clamped to MAX");
    expectEq( abrSwitchAdoptKbps(9999u, MIN, 1600u), 1600u,
              "carry above a lowered maxAbr -> clamped to that max");

    // ── adopt: max == 0 means "no upper bound" (do not zero the carry) ─────
    expectEq( abrSwitchAdoptKbps(1200u, MIN, 0u), 1200u,
              "max 0 -> no upper clamp, carry preserved");
    expectEq( abrSwitchAdoptKbps(0u, MIN, 0u), MIN,
              "max 0 + carry 0 -> still floored at MIN");

    // ── debounce: half-open window [lastMs, lastMs+debounce) ──────────────
    expectBool( abrSwitchDebounced(1000, 0, DEBOUNCE), true,
                "1000ms since last switch (< debounce) -> debounced");
    expectBool( abrSwitchDebounced(DEBOUNCE - 1, 0, DEBOUNCE), true,
                "one ms before the window closes -> debounced");
    expectBool( abrSwitchDebounced(DEBOUNCE, 0, DEBOUNCE), false,
                "exactly debounce ms elapsed -> NOT debounced (boundary)");
    expectBool( abrSwitchDebounced(DEBOUNCE + 1, 0, DEBOUNCE), false,
                "just past the window -> NOT debounced");
    expectBool( abrSwitchDebounced(5000, 5000, DEBOUNCE), true,
                "elapsed 0 after a switch -> debounced");
    expectBool( abrSwitchDebounced(5000, 3500, DEBOUNCE), true,
                "elapsed 1500 < debounce -> debounced (just-inside window)");
    expectBool( abrSwitchDebounced(5000, 10, DEBOUNCE), false,
                "elapsed 4990 > debounce -> NOT debounced");

    if (g_failures == 0) {
        std::printf("\nPASS: all switch-adopt checks passed\n");
        return 0;
    }
    std::printf("\nFAIL: %d check(s) failed\n", g_failures);
    return 1;
}
