#ifndef ABRSCORECLIFF_H
#define ABRSCORECLIFF_H

#include "ABRTuning.h"

// ══════════════════════════════════════════════════════════════════════════
//  ABRScoreCliff.h — Phase 12 (PORT DEVIATION): score-cliff detection math
//
//  The pure predicate behind the "video score collapsed" restart request, split
//  out of XBAdaptiveBitrateStreaming so it can be unit-tested WITHOUT a socket
//  (tests/tst_abr_score_cliff.cpp). No state, no Qt, no I/O — one free function.
//
//  The router's VIDEO score (row [17], 0..100) collapses when it falls well below
//  its RECENT PEAK. The caller maintains the peak as max() over the last
//  ABR_SCORE_CLIFF_WINDOW_TICKS scores (robust to a single noisy tick) and passes
//  it in here. A collapse means the stream is likely wedged (TCP retransmit
//  head-of-line blocking); the caller then asks for a video-socket restart, the
//  only userspace fix for that condition.
//
//    cliff  <=>  (baseline - score) >= ABR_SCORE_CLIFF_DELTA
//
//  Boundary is INCLUSIVE: exactly ABR_SCORE_CLIFF_DELTA counts as a cliff.
// ══════════════════════════════════════════════════════════════════════════

inline bool abrScoreCliff(double baseline, double score, double delta)
{
    return (baseline - score) >= delta;
}

#endif // ABRSCORECLIFF_H
