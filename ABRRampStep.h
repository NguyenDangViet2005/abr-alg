#ifndef ABRRAMPSTEP_H
#define ABRRAMPSTEP_H

#include "ABRTuning.h"

// ══════════════════════════════════════════════════════════════════════════
//  ABRRampStep.h — qdelay-aware ramp STEP math
//
//  The pure core of the "climb faster when the sender queue is empty, climb
//  cautiously when it is starting to fill" ramp change, split out of
//  XBAdaptiveBitrateStreaming so it can be unit-tested WITHOUT a socket
//  (tests/tst_abr_ramp_tuning.cpp). No state, no Qt, no I/O — one free function.
//
//  WHY: the ramp step was a fixed +ABR_BITRATE_STEP_KBPS (100) on every clean
//  tick, so a healthy link took ~8 s (5 steps x 2 s cooldown) to go 110 -> 510.
//  A LOW sender queue means the buffer is not building, so there is room to
//  climb two steps at once (+ABR_RAMP_STEP_FAST_KBPS 200).
//
//  HONEST SCOPE — SLOW IS CURRENTLY UNREACHABLE. The only call site
//  (XBAdaptiveBitrateStreaming::checkSenderQueue) reaches this helper INSIDE the
//  ramp branch, whose entry gate is `queuing_delay_ms < increase_q_delay_threshold`
//  with threshold = ABR_QUEUE_DELAY_LOW_THRESHOLD_MS (100) or, right after a
//  decrease, ABR_QUEUE_DELAY_LOW_AFTER_DECREASE_MS (20). Both are <= the 100 ms
//  boundary here, so at that site the helper ALWAYS returns FAST and the effective
//  behaviour is a fixed +200. SLOW (100) is therefore DEAD in production today —
//  the moderate band [LOW, HIGH) is a HOLD in the caller, never a ramp.
//
//  Kept (rather than collapsed to the single FAST macro) for two reasons: (1) the
//  plan requires a unit-testable step selector, and an inline ternary in the
//  caller cannot be tested without a socket; (2) it reserves the cautious tier
//  for a future caller whose qdelay may legitimately sit in [LOW, HIGH) and still
//  ramp. Do NOT describe SLOW as live behaviour in the caller's comments.
//
//  BOUNDARY: compared against ABR_QUEUE_DELAY_LOW_THRESHOLD_MS (100 ms), STRICTLY
//  below -> FAST; exactly 100.0 -> SLOW.
//
//  Sign/type: the ABR_* macros are UNTYPED int; the argument is double, so the
//  compare casts explicitly and the returns cast to unsigned int — no
//  -Wsign-compare, no bad template deduction.
// ══════════════════════════════════════════════════════════════════════════

inline unsigned int abrRampStepKbps(double queuingDelayMs)
{
    const bool low = queuingDelayMs < static_cast<double>(ABR_QUEUE_DELAY_LOW_THRESHOLD_MS);
    return low ? static_cast<unsigned int>(ABR_RAMP_STEP_FAST_KBPS)
               : static_cast<unsigned int>(ABR_RAMP_STEP_SLOW_KBPS);
}

#endif // ABRRAMPSTEP_H
