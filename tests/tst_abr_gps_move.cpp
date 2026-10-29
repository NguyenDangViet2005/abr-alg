// Unit test for the ABR GPS move predicate, ABRGpsMove.h (Phase 13).
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure — same style as tests/tst_abr_score_cliff.cpp. The
// helper is pure C++ (one free function), so no Qt types are needed.
//
// Build (adjust for your compiler):
//   g++ -std=c++17 -I.. tst_abr_gps_move.cpp -o tst_abr_gps_move
//
// The project rule is that the user builds; this file only has to be correct.

#include "../ABRGpsMove.h"
#include <cstdio>
#include <cmath>

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

// ~111320 m per degree of latitude. Used to build a pair a known distance apart
// so the test pins the MATH without depending on the current threshold knob.
static const double kMetersPerDegLat = 111320.0;

int main()
{
    const double lat0 = 10.776890, lon0 = 106.700810;   // HCM

    // ── same point ────────────────────────────────────────────────────────
    expectTrue(haversineMeters(lat0, lon0, lat0, lon0) < 1e-6,
               "same point -> 0 m");

    // ── a known ~500 m pair (north) ───────────────────────────────────────
    // 500 m north = 500 / 111320 degrees of latitude.
    const double dLat500 = 500.0 / kMetersPerDegLat;
    const double d500 = haversineMeters(lat0, lon0, lat0 + dLat500, lon0);
    expectTrue(std::fabs(d500 - 500.0) < 5.0, "~500 m north pair reads ~500 m");

    // ── straddle the 500 m threshold (both sides) ─────────────────────────
    const double d400 = haversineMeters(lat0, lon0, lat0 + 400.0 / kMetersPerDegLat, lon0);
    const double d600 = haversineMeters(lat0, lon0, lat0 + 600.0 / kMetersPerDegLat, lon0);
    expectTrue(d400 < 500.0, "400 m move is BELOW the 500 m gate");
    expectTrue(d600 > 500.0, "600 m move is ABOVE the 500 m gate");

    // ── symmetry (a->b == b->a) ───────────────────────────────────────────
    const double ab = haversineMeters(lat0, lon0, lat0 + dLat500, lon0 + dLat500);
    const double ba = haversineMeters(lat0 + dLat500, lon0 + dLat500, lat0, lon0);
    expectTrue(std::fabs(ab - ba) < 1e-6, "distance is symmetric");

    // ── longitude scales by cos(lat) — a 1-deg lon move is shorter than 1-deg lat
    const double dLon = haversineMeters(lat0, lon0, lat0, lon0 + 1.0);
    const double dLat = haversineMeters(lat0, lon0, lat0 + 1.0, lon0);
    expectTrue(dLon < dLat, "1 deg longitude < 1 deg latitude (cos(lat) scaling)");

    // ── finite / no NaN on an extreme-but-valid pair ──────────────────────
    const double dFar = haversineMeters(-89.0, 0.0, 89.0, 0.0);
    expectTrue(std::isfinite(dFar) && dFar > 0.0, "antipodal-ish pair is finite and positive");

    std::printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
