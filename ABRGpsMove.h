#ifndef ABRGPSMOVE_H
#define ABRGPSMOVE_H

#include <cmath>

// ══════════════════════════════════════════════════════════════════════════
//  ABRGpsMove.h — Phase 13 (PORT DEVIATION): great-circle move math
//
//  The pure predicate behind the GPS ceiling-escape gate, split out of
//  NetworkReference so it can be unit-tested WITHOUT a socket
//  (tests/tst_abr_gps_move.cpp). No state, no Qt, no I/O — one free function.
//
//  When the vehicle has moved ≥ ABR_GPS_MOVE_METERS (500 m) since the ceiling
//  was learned, the old ceiling is stale (new location = new radio environment)
//  and the ABR may lift it. The caller keeps the anchor position (snapshotted at
//  ceiling-learn) and passes both points in here.
//
//  The move THRESHOLD lives in ABRTuning.h (ABR_GPS_MOVE_METERS), NOT here: the
//  helper is pure math, the policy value is a tuning macro.
// ══════════════════════════════════════════════════════════════════════════

inline double haversineMeters(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusM = 6371000.0;   // mean radius
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

    const double phi1 = lat1 * kDegToRad;
    const double phi2 = lat2 * kDegToRad;
    const double dPhi = (lat2 - lat1) * kDegToRad;
    const double dLam = (lon2 - lon1) * kDegToRad;

    const double a = std::sin(dPhi / 2.0) * std::sin(dPhi / 2.0)
                   + std::cos(phi1) * std::cos(phi2)
                     * std::sin(dLam / 2.0) * std::sin(dLam / 2.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthRadiusM * c;
}

#endif // ABRGPSMOVE_H
