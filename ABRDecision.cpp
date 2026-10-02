#include "ABRDecision.h"

// Clamp helpers. Kept local and named so the branches below read as intent
// ("halve, floor at MIN") rather than arithmetic. Only POSITIVE results pass
// through these — QOS_PAUSE returns before any clamp, or it would be raised to
// MIN_BITRATE and unpause the video.
namespace {

int clampBitrate(int value, int maxAbr)
{
    if (value < ABR_MIN_BITRATE_KBPS) return ABR_MIN_BITRATE_KBPS;
    if (value > maxAbr)               return maxAbr;
    return value;
}

int clampHalve(int bitrate, int maxAbr) { return clampBitrate(bitrate / 2, maxAbr); }
int clampMul(int bitrate, double f, int maxAbr) { return clampBitrate(static_cast<int>(bitrate * f), maxAbr); }
int clampAdd(int bitrate, int step, int maxAbr) { return clampBitrate(bitrate + step, maxAbr); }

} // namespace

int decideBitrate(const RouterInterfaceStat &v, const RouterInterfaceStat &c,
                  C2Mode c2Mode, int bitrate, int notUsableTicks,
                  qint64 sinceLastChangeMs, int maxAbr)
{
    // v1 reads only the C2 MODE (present/stale/never-seen/delay-only); the C2
    // row's own metrics are not consumed yet. Kept as a parameter so the
    // SELF/NETWORK severity split has its input the moment it is wired.
    Q_UNUSED(c);

    // ── Hard gate: link is down ──────────────────────────────────────────
    // A down link is a definitive verdict, so it overrides everything. Hold
    // for the first two ticks (a single dropped UDP row must not stop video),
    // then pause.
    if (!v.isConnected || !v.isMptcp) {
        if (notUsableTicks + 1 >= ABR_PAUSE_AFTER_TICKS) return QOS_PAUSE;
        return (bitrate > ABR_MIN_BITRATE_KBPS) ? bitrate : ABR_MIN_BITRATE_KBPS;
    }

    // Connected + mptcp but the router's own `usable` is false (no sample yet).
    // "Cannot measure" — do not emit, do not touch state.
    if (!v.usable) return QOS_NOT_AVAILABLE;

    // ── KTO gate: retransmit >= 2 pushed score to the floor ──────────────
    if (v.score <= 1.0) return clampHalve(bitrate, maxAbr);

    // ── DECREASE (checked before INCREASE) ───────────────────────────────
    // A tick raises OR lowers, never both — the if/elif order below is what
    // guarantees that. Graduated, and the ORDER is the policy:
    //   loss   -> halve (harsh: the pipe is already dropping)
    //   qdelay -> ×0.8  (gentle: bufferbloat, delay-based backoff)
    //   trend  -> ×0.85 (earliest brake, score already falling fast)
    //
    // An earlier draft fused loss+qdelay into one compound branch whose inner
    // term ALSO tested `qdelay > ABR_QDELAY_HIGH_MS`. Because the outer test
    // used that same threshold, the compound fired for every qdelay>400 case
    // and made the ×0.8 line UNREACHABLE — dead code that silently promoted a
    // gentle backoff to a hard halve. The two loss tiers below both halve for
    // now; they stay separate so a stronger response (drop-to-MIN) has a home
    // when tuned. Report §7 lists qdelay>400 as ×0.8, which this restores.
    if (v.lossRatePct > ABR_LOSS_SEVERE_PCT)   return clampHalve(bitrate, maxAbr);
    if (v.lossRatePct > ABR_LOSS_HIGH_PCT)     return clampHalve(bitrate, maxAbr);
    if (v.queuingDelayMs > ABR_QDELAY_HIGH_MS) return clampMul(bitrate, ABR_QDELAY_GAIN, maxAbr);
    if (v.trend == TREND_RAPID_DECREASE)       return clampMul(bitrate, ABR_TREND_RAPID_DEC_GAIN, maxAbr);

    // ── INCREASE ─────────────────────────────────────────────────────────
    // Permitted only when C2 gives a reference (FRESH) or C2 is not deployed
    // at all (DELAY_ONLY). STALE/NEVER_SEEN withhold the reference and block
    // the ramp — never the decrease.
    const bool c2AllowsUp = (c2Mode == C2Mode::FRESH || c2Mode == C2Mode::DELAY_ONLY);
    const bool linkClean  = v.queuingDelayMs < ABR_QDELAY_LOW_MS
                            && (v.rttMs - v.minRttMs) < ABR_RTT_INFLATION_MS
                            && v.jitterMs < ABR_JITTER_HIGH_MS;
    if (c2AllowsUp
        && v.score >= ABR_SCORE_UP_MIN
        && v.trend <= TREND_STABLE          // improving/stable, NOT decreasing
        && bitrate < maxAbr
        && linkClean
        && sinceLastChangeMs >= ABR_UP_COOLDOWN_MS)
        return clampAdd(bitrate, ABR_UP_STEP_KBPS, maxAbr);

    return bitrate;   // HOLD — a positive value, distinct from NOT_AVAILABLE
}
