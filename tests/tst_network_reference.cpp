// Unit test for NetworkReference (Phase 13: strength + GPS environment gate).
//
// NetworkReference uses QElapsedTimer, so unlike the pure-header tests this one
// needs Qt. Build (adjust for your Qt install):
//   g++ -std=c++17 -fPIC $(pkg-config --cflags Qt6Core) tst_network_reference.cpp \
//       ../NetworkReference.cpp $(pkg-config --libs Qt6Core) -o tst_network_reference
//
// The time-DEPENDENT branches (5 s staleness) are NOT exercised here — they need
// real sleeps and a time seam the class does not expose. This pins the
// deterministic logic: anchor seeding, the haversine move gate, and the strength
// regression. The project rule is that the user builds; this file must be correct.

#include "../NetworkReference.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;

static void expectTrue(bool ok, const char *what)
{
    if (!ok) { ++g_failures; std::printf("FAIL %s\n", what); }
    else     { std::printf("ok   %s\n", what); }
}

static const double kMetersPerDegLat = 111320.0;
static const double LAT0 = 10.776890, LON0 = 106.700810;

int main()
{
    // ── GPS: learned with NO fix → first fix seeds the anchor (no auto-lift) ─
    {
        NetworkReference r;
        r.markBaseline();                       // ceiling learned, no 604 yet
        expectTrue(!r.environmentChanged(), "no fix after blind learn -> no lift");

        r.setGps(LAT0, LON0);                    // first fix seeds anchor at THIS point
        expectTrue(!r.environmentChanged(), "first fix seeds anchor -> NOT a lift");

        // A later fix ≥500 m away lifts.
        r.setGps(LAT0 + 600.0 / kMetersPerDegLat, LON0);
        expectTrue(r.environmentChanged(), "600 m from seeded anchor -> lift");
    }

    // ── GPS: learned WITH a fix → move measured from the learn position ──────
    {
        NetworkReference r;
        r.setGps(LAT0, LON0);
        r.markBaseline();                        // anchor = LAT0,LON0
        expectTrue(!r.environmentChanged(), "no move since learn -> no lift");

        r.setGps(LAT0 + 400.0 / kMetersPerDegLat, LON0);
        expectTrue(!r.environmentChanged(), "400 m < 500 m -> no lift");

        r.setGps(LAT0 + 500.0 / kMetersPerDegLat, LON0);
        expectTrue(r.environmentChanged(), "500 m >= gate -> lift");

        r.setGps(LAT0 + 500.0 / kMetersPerDegLat, LON0);   // stationary at the far point
        expectTrue(r.environmentChanged(), "still >=500 m from anchor -> still lift");
    }

    // ── GPS: never received a 604 → fail-closed ─────────────────────────────
    {
        NetworkReference r;
        r.markBaseline();
        expectTrue(!r.environmentChanged(), "never got a 604 -> no lift");
        expectTrue(!r.hasUsableGps(), "never got a 604 -> hasUsableGps false");
    }

    // ── GPS: out-of-range fix is dropped (trust boundary) ───────────────────
    {
        NetworkReference r;
        r.markBaseline();
        r.setGps(999.0, 999.0);                  // invalid → dropped
        expectTrue(!r.hasUsableGps(), "out-of-range fix dropped -> no usable GPS");
    }

    // ── strength regression: unchanged behaviour ────────────────────────────
    {
        NetworkReference r;
        r.setStrength(1);
        r.markBaseline();                        // baseline = 1
        r.setStrength(2);                        // improved
        for (int i = 0; i < ABR_STRENGTH_CONFIRM_TICKS; ++i) r.setStrength(2);
        expectTrue(r.environmentChanged(), "strength 1->2 held -> lift (regression)");
    }
    {
        NetworkReference r;
        r.setStrength(4);
        r.markBaseline();                        // baseline = 4 (max)
        for (int i = 0; i < ABR_STRENGTH_CONFIRM_TICKS + 2; ++i) r.setStrength(4);
        expectTrue(!r.environmentChanged(), "strength stuck at 4 -> no lift (regression)");
    }

    // ── reset clears GPS state ──────────────────────────────────────────────
    {
        NetworkReference r;
        r.setGps(LAT0, LON0);
        r.markBaseline();
        r.setGps(LAT0 + 600.0 / kMetersPerDegLat, LON0);
        expectTrue(r.environmentChanged(), "pre-reset: moved -> lift");
        r.reset();
        expectTrue(!r.environmentChanged(), "post-reset: forgotten -> no lift");
        expectTrue(!r.hasUsableGps(), "post-reset: no usable GPS");
    }

    std::printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
