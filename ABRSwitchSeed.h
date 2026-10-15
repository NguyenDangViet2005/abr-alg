#ifndef ABRSWITCHSEED_H
#define ABRSWITCHSEED_H

#include "ABRTuning.h"

// ══════════════════════════════════════════════════════════════════════════
//  ABRSwitchSeed.h — Phase 13 (PORT DEVIATION): switch-seed decision math
//
//  The pure core of the "a spare just became primary, seed it off the floor"
//  fix, split out of XBAdaptiveBitrateStreaming so it can be unit-tested
//  WITHOUT a socket (tests/tst_abr_switch_seed.cpp). No state, no Qt, no I/O —
//  two free functions.
//
//  WHY: a spare interface's ABR instance ramps in the background like everyone
//  else, so by the time the router promotes it the loss ladder may have left it
//  PINNED at MIN even though its link has since recovered. The old switch path
//  force-dispatched that stale floor value (the eth-weak -> eth-strong
//  under-shoot: camera 1200 -> 30 in one tick, then a ~22 s climb back). The
//  seed lifts a floor-locked link by evidence instead.
//
//  Rules (keys STRICTLY on being at the floor):
//    • current > MIN            -> unchanged (the normal ramp owns it)
//    • current <= MIN, healthy  -> ABR_SWITCH_SEED_HEALTHY_KBPS (min cap)
//    • current <= MIN, not      -> ABR_SWITCH_SEED_FLOOR_KBPS   (min cap)
//  The seed is always min()-clamped by the learned cap and floored at MIN, so it
//  can never exceed what this link was shown to carry nor dip below the floor.
//
//  "Healthy" is the SAME evidence the ramp gate trusts, read from the
//  link's own latest VIDEO row: router says usable, no loss THIS tick, score at
//  or above the step-up bar, queuing delay below the good threshold.
//
//  Sign/type: ABR_* macros are UNTYPED int; the params are unsigned/double, so
//  every compare casts explicitly — no -Wsign-compare, no bad deduction.
// ══════════════════════════════════════════════════════════════════════════

inline bool abrSwitchLinkHealthy(bool usable, bool lossThisTick,
                                 double score, double queuingDelayMs)
{
    return usable
        && !lossThisTick
        && score >= static_cast<double>(ABR_OVERSHOOT_MIN_SCORE_AT_STEPUP)
        && queuingDelayMs < static_cast<double>(ABR_QUEUE_DELAY_LOW_THRESHOLD_MS);
}

inline unsigned int abrSwitchSeedKbps(unsigned int currentKbps, unsigned int capKbps,
                                      bool healthy)
{
    const unsigned int floorKbps = static_cast<unsigned int>(ABR_MIN_BITRATE_KBPS);
    // Not pinned at the floor: the spare already sits where it should — leave it
    // EXACTLY as-is (a bootstrap jump here would buy nothing).
    if (currentKbps > floorKbps) return currentKbps;

    const unsigned int target = healthy
        ? static_cast<unsigned int>(ABR_SWITCH_SEED_HEALTHY_KBPS)
        : static_cast<unsigned int>(ABR_SWITCH_SEED_FLOOR_KBPS);
    // Never exceed the learned steady cap; never dip below the floor.
    unsigned int seeded = (target < capKbps) ? target : capKbps;
    if (seeded < floorKbps) seeded = floorKbps;
    return seeded;
}

#endif // ABRSWITCHSEED_H
