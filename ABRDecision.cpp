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

// A decision with its branch/reason attached, so each return below reads as the
// verdict it is rather than a bare number a later reader has to re-derive.
AbrDecision verdict(int kbps, AbrBranch branch, AbrReason reason)
{
    return AbrDecision{ kbps, branch, reason };
}

} // namespace

AbrDecision decideBitrate(const RouterInterfaceStat &v, const RouterInterfaceStat &c,
                          C2Mode c2Mode, int bitrate, const AbrCounters &cnt,
                          bool cameraCollapsed, qint64 sinceLastChangeMs, int maxAbr,
                          int rampCapKbps)
{
    // ── Hard gate: link is down ──────────────────────────────────────────
    // A down link is a definitive verdict, so it overrides everything. Hold
    // for the first two ticks (a single dropped UDP row must not stop video),
    // then pause.
    if (!v.isConnected || !v.isMptcp) {
        if (cnt.notUsableTicks + 1 >= ABR_PAUSE_AFTER_TICKS)
            return verdict(QOS_PAUSE, AbrBranch::PAUSE, AbrReason::LINK_DEAD);
        return verdict((bitrate > ABR_MIN_BITRATE_KBPS) ? bitrate : ABR_MIN_BITRATE_KBPS,
                       AbrBranch::HOLD, AbrReason::LINK_DEAD);
    }

    // Connected + mptcp but the router's own `usable` is false (no sample yet).
    // "Cannot measure" — do not emit, do not touch state.
    if (!v.usable)
        return verdict(QOS_NOT_AVAILABLE, AbrBranch::NOT_AVAILABLE, AbrReason::NOT_USABLE);

    // ── KTO gate: retransmit >= 2 pushed score to the floor ──────────────
    if (v.score <= 1.0)
        return verdict(clampHalve(bitrate, maxAbr), AbrBranch::HALVE, AbrReason::KTO);

    // ── C2-loss gate: the PATH is dropping, stop the video ───────────────
    //
    // The C2 row is the only loss signal that is not TCP-hidden: the video
    // flow's own lossRatePct is masked by retransmits, so a genuinely lossy
    // path shows up here and nowhere else. Above the threshold the path is
    // dropping faster than any bitrate can cope with, so a halve (what the
    // video's own loss branch below does) just sends less into the same lossy
    // wire — pause instead.
    //
    // FRESH only. A STALE or NEVER_SEEN C2 is "unknown", and unknown loss must
    // NOT pause the video: the same discipline as c2AllowsUp, where a missing
    // reference blocks the ramp but never the decrease. c2LossTicks is the
    // debounce (the caller counts consecutive lossy ticks), so one noisy sample
    // cannot stop the stream; a success on any tick clears it.
    if (c2Mode == C2Mode::FRESH
        && c.lossRatePct > ABR_C2_LOSS_PAUSE_PCT
        && cnt.c2LossTicks + 1 >= ABR_C2_LOSS_PAUSE_TICKS)
        return verdict(QOS_PAUSE, AbrBranch::PAUSE, AbrReason::C2_LOSS_HIGH);

    // ── DECREASE (checked before INCREASE) ───────────────────────────────
    // A tick raises OR lowers, never both — the if/elif order below is what
    // guarantees that. The ORDER is the policy, and it is TWO-SPEED:
    //   loss  -> halve  (FAST, harsh: the pipe is already dropping)
    //   trend -> ×0.85  (FAST, earliest brake, score already falling fast)
    //   score streak low -> ×0.80 (SLOW: 3 low samples in a row)
    //
    // The fast branches come FIRST so a real loss/trend event is answered on the
    // tick it happens and the slow branch is never reached on that same tick —
    // no ÷2 stacked with ×0.80. The slow branch is the LOSS-FREE brake: a link
    // decaying without dropping packets reaches the decision only here.
    //
    // There is NO separate queue/congestion branch. Score already carries the
    // queue: computeRawScore charges wRTT*(rtt-minRtt) plus loss and jitter, so
    // a filling buffer lowers score, which drags the streak low. A dedicated
    // rtt-minRtt backoff used to sit here and was removed (2026-10-06) — it was
    // the sibling of the router's Little's-Law queuingDelayMs, a feedback trap
    // (its divisor is the throughput the ABR sets). One aggregate signal (score)
    // decides, exactly as the pre-refactor routerModule did.
    //
    // The two loss tiers both halve for now; they stay separate so a stronger
    // response (drop-to-MIN) has a home when tuned.
    if (v.lossRatePct > ABR_LOSS_SEVERE_PCT)
        return verdict(clampHalve(bitrate, maxAbr), AbrBranch::HALVE, AbrReason::LOSS_HIGH);
    if (v.lossRatePct > ABR_LOSS_HIGH_PCT)
        return verdict(clampHalve(bitrate, maxAbr), AbrBranch::HALVE, AbrReason::LOSS_HIGH);
    if (v.trend == TREND_RAPID_DECREASE)
        return verdict(clampMul(bitrate, ABR_TREND_RAPID_DEC_GAIN, maxAbr),
                       AbrBranch::MUL, AbrReason::TREND_RAPID_DEC);
    // Slow path: the last ABR_SCORE_STREAK_TICKS samples were all below
    // ABR_SCORE_DEC_MIN. The streak is counted by the wrapper, so this is a pure
    // test. Fires only when the two fast branches above did not.
    if (cnt.scoreLowStreak >= ABR_SCORE_STREAK_TICKS)
        return verdict(clampMul(bitrate, ABR_SCORE_DEC_GAIN, maxAbr),
                       AbrBranch::MUL, AbrReason::SCORE_LOW);

    // ── INCREASE ─────────────────────────────────────────────────────────
    // Permitted only when C2 gives a reference (FRESH) or C2 is not deployed
    // at all (DELAY_ONLY). STALE/NEVER_SEEN withhold the reference and block
    // the ramp — never the decrease.
    const bool c2AllowsUp = (c2Mode == C2Mode::FRESH || c2Mode == C2Mode::DELAY_ONLY);
    // The ramp gate is the HIGH STREAK, not a single sample (2026-10-06): the
    // last ABR_SCORE_STREAK_TICKS scores were all >= ABR_SCORE_UP_MIN. This
    // replaces both the single-sample `score >= ABR_SCORE_UP_MIN` test and the
    // `trend <= TREND_STABLE` gate — the streak IS our trend. Score remains the
    // SINGLE quality signal (computeRawScore already charges wRTT*(rtt-minRtt),
    // loss and jitter), so a filling queue, dropping packets or growing jitter
    // all drag the streak low and hold the ramp shut.
    //
    // rampCapKbps is the ceiling memory (see ABR_CEILING_*). It gates the ramp
    // ONLY — a decrease above is never capped, because lowering is exactly what a
    // link at its wall needs. `<= 0` means no memory is live. The test is on the
    // PROPOSED step, so a ramp PARKS at the cap (equal is allowed) instead of
    // overshooting it by one step.
    if (c2AllowsUp
        && cnt.scoreHighStreak >= ABR_SCORE_STREAK_TICKS
        && bitrate < maxAbr
        && !cameraCollapsed                 // camera not delivering what we ask
        && sinceLastChangeMs >= ABR_UP_COOLDOWN_MS
        && (rampCapKbps <= 0 || bitrate + ABR_UP_STEP_KBPS <= rampCapKbps))
        return verdict(clampAdd(bitrate, ABR_UP_STEP_KBPS, maxAbr), AbrBranch::UP, AbrReason::COOLDOWN_OK);

    // HOLD — a positive value, distinct from NOT_AVAILABLE. A hold is the
    // steady state, so its reason is NONE unless a gate definitively withheld
    // the ramp: a withheld C2 reference (STALE), a score below the ramp floor
    // (SCORE_LOW), a score AT/above the floor whose streak has not yet reached
    // the required length (SCORE_HIGH), or the ceiling cap (CEILING). Naming any
    // other gate here would be a guess the wire enum does not back, so it stays
    // NONE. The CEILING test mirrors the ramp gate exactly: it is only reached
    // when everything else would have permitted an INCREASE.
    AbrReason holdReason = AbrReason::NONE;
    if (!c2AllowsUp && c2Mode == C2Mode::STALE) holdReason = AbrReason::STALE;
    else if (c2AllowsUp && v.score < ABR_SCORE_UP_MIN) holdReason = AbrReason::SCORE_LOW;
    else if (c2AllowsUp && cnt.scoreHighStreak < ABR_SCORE_STREAK_TICKS)
        holdReason = AbrReason::SCORE_HIGH;
    else if (c2AllowsUp && rampCapKbps > 0 && bitrate + ABR_UP_STEP_KBPS > rampCapKbps)
        holdReason = AbrReason::CEILING;
    return verdict(bitrate, AbrBranch::HOLD, holdReason);
}
