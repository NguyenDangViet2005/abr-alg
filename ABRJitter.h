#ifndef ABRJITTER_H
#define ABRJITTER_H

#include "ABRTuning.h"
#include <cmath>

// ══════════════════════════════════════════════════════════════════════════
//  ABRJitter.h — pure jitter (RTT stddev) math for the ABR ramp gate
//
//  The decision core of the jitter gate, split out of XBAdaptiveBitrateStreaming
//  so it can be unit-tested WITHOUT a socket (tests/tst_abr_ramp_tuning.cpp).
//  No state, no Qt, no I/O — one free template function.
//
//  DEFINITION: jitter = POPULATION standard deviation of the last N RTT samples
//  (N = ABR_RTT_HISTORY_SIZE). This is xbfirm's formula, verbatim: mean over the
//  window, squared deviations, sqrt of the mean square. The caller feeds the
//  window (m_rttHistory, one sample per ramp tick) and compares the result to
//  ABR_JITTER_STABLE_THRESHOLD_MS; jitter BELOW the threshold opens the ramp gate.
//
//  WINDOW SIZE IS THE LEVER (2026-10-16): the value only "remembers" a spike for
//  N samples, so shrinking N from 10 to 5 makes the gate reopen ~2 ticks sooner
//  after a transient and roughly doubles the number of clean ticks on a link that
//  is mostly good (measured: eth1 14->29, eth2 12->24 clean ticks / 45). MAD
//  (median/mean absolute deviation) was evaluated as an alternative and REJECTED:
//  on a genuine multi-sample congestion spike it collapsed to ~1ms (MAD-median)
//  and would have opened the gate into a 321 ms-stalled link.
//
//  Type: templated on the sample type so a Qt-free header can accept a plain
//  array in a test and `m_rttHistory.constData()` (quint32) in production — no
//  Qt include leaks into this header. count < 2 -> 0.0 (a single sample has no
//  spread; matches the old calculateJitter() early-out).
// ══════════════════════════════════════════════════════════════════════════

template <typename SampleT>
inline double abrJitterStddev(const SampleT *samples, int count)
{
    if (count < 2) return 0.0;

    double sum = 0.0;
    for (int i = 0; i < count; ++i) sum += static_cast<double>(samples[i]);
    const double mean = sum / static_cast<double>(count);

    double sq_sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const double d = static_cast<double>(samples[i]) - mean;
        sq_sum += d * d;   // == qPow(d, 2), without the Qt dependency
    }
    return std::sqrt(sq_sum / static_cast<double>(count));   // population stddev
}

#endif // ABRJITTER_H
