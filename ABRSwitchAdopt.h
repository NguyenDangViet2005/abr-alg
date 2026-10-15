#ifndef ABRSWITCHADOPT_H
#define ABRSWITCHADOPT_H

#include "ABRTuning.h"

// ══════════════════════════════════════════════════════════════════════════
//  ABRSwitchAdopt.h — primary-role bitrate ADOPT decision math
//
//  The pure core of the "on a primary switch, the new primary ADOPTS the
//  effective bitrate the camera is already running" fix, split out of
//  XBAdaptiveBitrateStreaming / ABRFactory so it can be unit-tested WITHOUT a
//  socket (tests/tst_abr_switch_adopt.cpp). No state, no Qt, no I/O — two free
//  functions.
//
//  WHY: bitrate is a property of the PRIMARY ROLE, not of a per-interface
//  ladder. A spare's instance keeps ingesting its row (the HUD stays warm) but
//  no longer adapts — its ladder is FROZEN. On the A->B edge the new primary
//  takes the effective bitrate (the value the camera is already running, i.e.
//  XBQoSService::m_currentBitrate, which the factory caches) and then its own
//  normal ABR continues from there. This is the fix for the eth-weak ->
//  eth-strong under-shoot: the old path force-dispatched B's stale internal
//  value (a spare ladder that had collapsed itself to 30), dropping the camera
//  1200 -> 30 in one tick with a ~22 s climb back. Now the camera stays at the
//  value it held across the switch.
//
//  abrSwitchAdoptKbps — CLAMP the carried value into [minKbps, maxKbps]. The
//  carry is a real, in-range bitrate the camera was running, so the clamp is a
//  SAFETY, not a correction: it can only fire on a pathological carry (a stale
//  0 from before the first dispatch, or a value above a since-lowered maxAbr).
//  maxKbps == 0 (no max set) is treated as "no upper bound" so a startup carry
//  is not clamped to nothing.
//
//  abrSwitchDebounced — anti-jitter for the switch itself. A flip-flop that
//  reverses inside the debounce window does NOT re-adopt, so a router that
//  bounces A->B->A in one interval cannot reseed the camera back and forth.
//  Takes `long long` (monotonic elapsed ms) so the header stays Qt-free; the
//  boundary is INCLUSIVE on the debounce upstream: exactly debounce ms elapsed
//  is NOT debounced (window is half-open, [lastMs, lastMs+debounce)).
//
//  Sign/type: ABR_* macros are UNTYPED int; params are unsigned/long long, so
//  every compare casts explicitly — no -Wsign-compare, no bad deduction.
// ══════════════════════════════════════════════════════════════════════════

inline unsigned int abrSwitchAdoptKbps(unsigned int carryKbps,
                                       unsigned int minKbps,
                                       unsigned int maxKbps)
{
    unsigned int adopted = carryKbps;
    // maxKbps == 0 means "no upper bound configured" — do not clamp the carry to
    // nothing (a startup carry must survive).
    if (maxKbps != 0u && adopted > maxKbps) adopted = maxKbps;
    if (adopted < minKbps) adopted = minKbps;
    return adopted;
}

inline bool abrSwitchDebounced(long long nowMs, long long lastSwitchMs, long long debounceMs)
{
    return (nowMs - lastSwitchMs) < debounceMs;
}

#endif // ABRSWITCHADOPT_H
