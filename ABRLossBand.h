#ifndef ABRLOSSBAND_H
#define ABRLOSSBAND_H

#include "ABRTuning.h"

// ══════════════════════════════════════════════════════════════════════════
//  ABRLossBand.h — Phase 11 (PORT DEVIATION): LOSS LADDER band math
//
//  The pure decision core of the graduated loss ladder, split out of
//  XBAdaptiveBitrateStreaming so it can be unit-tested WITHOUT a socket
//  (tests/tst_abr_loss_band.cpp). No state, no Qt, no I/O — one free function.
//
//  Video loss% -> next bitrate (kbps) + a flag telling the caller to emit
//  requestStreamReset. The MIN floor is applied to the -STEP and *FACTOR bands.
//
//  | loss%      | action                                        |
//  |------------|-----------------------------------------------|
//  | <= 0.8     | clean (return current; caller returns false)  |
//  | 0.8 .. 20  | current - ABR_LOSS_MILD_STEP_KBPS (floor MIN) |
//  | 20 .. 50   | current * ABR_LOSS_HALVE_FACTOR   (floor MIN) |
//  | 50 .. 70   | MIN                                           |
//  | > 70       | MIN + isReset = true                          |
//
//  Band edges are INCLUSIVE on the upper bound (loss == 20 -> mild,
//  loss == 50 -> halve, loss == 70 -> MIN-without-reset).
//
//  Sign/type: the ABR_* macros are UNTYPED (int); lossPct is double and
//  currentKbps is unsigned, so every compare against a macro is cast
//  explicitly — no -Wsign-compare / no bad template deduction.
// ══════════════════════════════════════════════════════════════════════════

inline unsigned int abrLossDecreaseAction(double lossPct, unsigned int currentKbps, bool &isReset)
{
    isReset = false;
    const unsigned int minKbps = static_cast<unsigned int>(ABR_MIN_BITRATE_KBPS);

    // Clean: at or below the trigger. The caller's entry gate already returns
    // false here; the helper mirrors it so the band table is self-contained.
    if (lossPct <= static_cast<double>(ABR_VIDEO_LOSS_TRIGGER_PCT)) {
        return currentKbps;
    }

    // Mild: subtract a fixed step, floored at MIN. (current <= step -> MIN.)
    if (lossPct <= static_cast<double>(ABR_LOSS_MILD_MAX_PCT)) {
        const unsigned int step = static_cast<unsigned int>(ABR_LOSS_MILD_STEP_KBPS);
        const unsigned int next = (currentKbps > step) ? (currentKbps - step) : 0u;
        return (next < minKbps) ? minKbps : next;
    }

    // Halve: scale by the shared factor, floored at MIN.
    if (lossPct <= static_cast<double>(ABR_LOSS_HALVE_MAX_PCT)) {
        const unsigned int next =
            static_cast<unsigned int>(currentKbps * static_cast<double>(ABR_LOSS_HALVE_FACTOR));
        return (next < minKbps) ? minKbps : next;
    }

    // Severe: MIN, no reset. The MIN band ends where the reset band begins —
    // ONE boundary macro (ABR_LOSS_RESET_PCT), used by both the helper and the
    // caller's log, so the two cannot drift.
    if (lossPct <= static_cast<double>(ABR_LOSS_RESET_PCT)) {
        return minKbps;
    }

    // Collapse: MIN + reset (the caller emits requestStreamReset).
    isReset = true;
    return minKbps;
}

#endif // ABRLOSSBAND_H
