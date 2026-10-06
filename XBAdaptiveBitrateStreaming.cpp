#include "XBAdaptiveBitrateStreaming.h"
#include <QDebug>
#include <QtMath>

static const char *c2ModeName(C2Mode m)
{
    switch (m) {
    case C2Mode::FRESH:      return "FRESH";
    case C2Mode::STALE:      return "STALE";
    case C2Mode::NEVER_SEEN: return "NEVER_SEEN";
    case C2Mode::DELAY_ONLY: return "DELAY_ONLY";
    }
    return "?";
}

XBAdaptiveBitrateStreaming::XBAdaptiveBitrateStreaming(const QString &iface,
                                                       bool isC2Configured,
                                                       QObject *parent)
    : IAdaptiveBitrateStreaming(parent)
    , m_iface(iface)
    , m_isC2Configured(isC2Configured)
    , m_maxAbr(ABR_DEFAULT_MAX_ABR_BITRATE)
{
    m_clock.start();
    qInfo().nospace() << "[ABR][" << m_iface << "] instance created (isC2Configured="
                      << m_isC2Configured << ", maxAbr=" << m_maxAbr << ")";
}

void XBAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{
    m_maxAbr = static_cast<int>(newMaxAbrBitrate);
    // A ceiling below the CURRENT bitrate would otherwise be violated on every
    // HOLD: the INCREASE gate requires `bitrate < maxAbr`, so a bitrate already
    // above the ceiling can never be raised OR lowered by the ramp — HOLD
    // returns it unchanged. Pull the current value down to the new ceiling now.
    // (ABR_INITIAL_BITRATE_KBPS is 250, so this only bites when a caller sets a
    // ceiling under the current ask; with the default 3000 it is a no-op.)
    if (m_bitrate > m_maxAbr) m_bitrate = m_maxAbr;
}

void XBAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    // Internal state reset only — NOT a socket reset. Mirrors the base
    // contract; the socket-reset path is a separate, unimplemented signal.
    m_bitrate = (bitrateKbps > 0) ? bitrateKbps : ABR_INITIAL_BITRATE_KBPS;
    // Same ceiling rule as setMaxAbrBitrate: a restore must not land above the
    // configured max, or HOLD would keep it there.
    if (m_bitrate > m_maxAbr) m_bitrate = m_maxAbr;
    m_notUsableTicks = 0;
    m_c2LossTicks = 0;
    m_scoreLowStreak = 0;
    m_scoreHighStreak = 0;
    // The ceiling memory belongs to the OLD stream; a reset starts a new one and
    // the wall may have moved. Drop it so the fresh stream is not capped.
    m_ceilingKbps = 0;
    m_ceilingSinceMs = 0;
    // The camera's last report belongs to the OLD stream; a reset starts a new
    // one, so a stale achieved value must not survive it.
    m_lastAchievedKbps = 0;
    m_cameraCollapsed = false;
    m_lastEmitted = -1;
    m_lastChangeMs = m_clock.elapsed();
    m_observing = false;
    m_obsTicks = 0;
}

void XBAdaptiveBitrateStreaming::handleRouterRow(const RouterInterfaceStat &row)
{
    // The factory routes by name, but a defensive check keeps a miswired row
    // from silently becoming this interface's data.
    if (row.interfaceName != m_iface) {
        qWarning().nospace() << "[ABR][" << m_iface << "] row for wrong iface '"
                             << row.interfaceName << "' — ignored";
        return;
    }

    if (row.parseFailed) {
        // A row whose fields could not be parsed is worse than a missing row:
        // it carries plausible zeros. Do not store it — treat as silence.
        qWarning().nospace() << "[ABR][" << m_iface << "] row parseFailed — not stored";
        return;
    }

    if (row.connType == ConnType::VIDEO) {
        m_video = row;
        m_lastVideoMs = m_clock.elapsed();
        m_videoEverSeen = true;
    } else if (row.connType == ConnType::C2) {
        m_c2 = row;
        m_lastC2Ms = m_clock.elapsed();
        m_c2EverSeen = true;
    }
}

void XBAdaptiveBitrateStreaming::handleCameraReportedBitrate(int achievedKbps)
{
    // A non-positive report means the camera said nothing (or reported 0 for an
    // unpaused stream) — it is NOT evidence of collapse. 0/requested would read
    // as ratio 0 < 0.25 and wrongly block the ramp, so ignore it entirely and
    // leave the previous verdict standing. Mirrors SRT's guard
    // (SRTAdaptiveBitrateStreaming.cpp:402).
    if (achievedKbps <= 0) return;
    if (m_bitrate <= 0) return;

    m_lastAchievedKbps = achievedKbps;
    const double ratio = static_cast<double>(achievedKbps) / static_cast<double>(m_bitrate);
    const bool collapsed = (ratio < ABR_CAMERA_COLLAPSE_RATIO);
    if (collapsed != m_cameraCollapsed) {
        qWarning().nospace() << "[ABR][" << m_iface << "] camera "
                             << (collapsed ? "COLLAPSED" : "recovered")
                             << " (asked=" << m_bitrate << "kbps achieved=" << achievedKbps
                             << "kbps ratio=" << QString::number(ratio, 'f', 2) << ")";
    }
    m_cameraCollapsed = collapsed;
}

bool XBAdaptiveBitrateStreaming::hasFreshVideoRow() const
{
    return m_lastVideoMs > m_lastComputeMs;
}

C2Mode XBAdaptiveBitrateStreaming::c2ModeForThisTick()
{
    if (!m_isC2Configured) return C2Mode::DELAY_ONLY;   // C2 not deployed → delay-based
    if (m_lastC2Ms > m_lastComputeMs) return C2Mode::FRESH;   // a C2 row arrived this tick
    if (m_c2EverSeen) return C2Mode::STALE;             // seen before, absent now
    return C2Mode::NEVER_SEEN;                          // declared present, never seen
}

AbrDecision XBAdaptiveBitrateStreaming::processQoS()
{
    // ── freshness guard (§6b) ────────────────────────────────────────────
    // No new VIDEO row since the last computation → recomputing would act on
    // already-seen numbers, and a since-expired cooldown could then RAISE the
    // bitrate on stale data. Skip.
    if (!hasFreshVideoRow()) {
        const qint64 silenceMs = (m_lastVideoMs < 0)
                                     ? m_clock.elapsed()
                                     : m_clock.elapsed() - m_lastVideoMs;
        if (silenceMs > ABR_STALE_WARN_MS && !m_staleWarned) {
            qWarning().nospace() << "[ABR][" << m_iface << "] silent " << silenceMs
                                 << "ms — holding bitrate " << m_bitrate << "kbps, not computing";
            m_staleWarned = true;
        }
        return AbrDecision{ QOS_NOT_AVAILABLE, AbrBranch::NOT_AVAILABLE, AbrReason::STALE };
    }
    m_staleWarned = false;

    // Resolve C2 freshness BEFORE stamping m_lastComputeMs. C2 rows always
    // arrive before the 602 tick marker, so m_lastC2Ms is older than "now";
    // if we stamped first, `m_lastC2Ms > m_lastComputeMs` could never hold and
    // FRESH would be unreachable — every tick would resolve STALE and the ramp
    // would be permanently blocked. The video guard above works only because it
    // reads the pre-update value; C2 must use the same pre-update value.
    const C2Mode c2Mode = c2ModeForThisTick();
    m_lastComputeMs = m_clock.elapsed();
    m_lastC2Mode = c2Mode;   // published with the decision in handleRouterTick

    // C2 declared present but never seen → warn once, after a grace period, so
    // a slow C2 subflow at startup is not mistaken for a misconfiguration.
    if (c2Mode == C2Mode::NEVER_SEEN && !m_c2Warned
        && m_clock.elapsed() > ABR_C2_GRACE_MS) {
        qWarning().nospace() << "[ABR][" << m_iface
                             << "] C2 declared configured but 601 never seen — blocking INCREASE";
        m_c2Warned = true;
    }

    const qint64 sinceLastChangeMs = m_clock.elapsed() - m_lastChangeMs;

    // Count consecutive link-down ticks from the LINK STATE, not from the
    // returned bitrate. Driving it from `b == QOS_PAUSE` deadlocks: the core
    // needs notUsableTicks+1 >= ABR_PAUSE_AFTER_TICKS to RETURN pause, but a
    // down link returns hold (positive), so the counter would reset to 0 every
    // tick and PAUSE could never fire — the video would never stop.
    const bool linkDown = !(m_video.isConnected && m_video.isMptcp);
    const int ticksBefore = m_notUsableTicks;
    m_notUsableTicks = linkDown ? m_notUsableTicks + 1 : 0;

    // Count consecutive FRESH-C2 ticks above the loss threshold, for the same
    // reason as notUsableTicks above: the core must see "before this tick" so it
    // can require N in a row. Only a FRESH C2 carries a live loss reading —
    // STALE/NEVER_SEEN is "unknown", not "clean", so it clears the streak rather
    // than letting it accrue on stale data.
    const int c2LossTicksBefore = m_c2LossTicks;
    const bool c2Lossy = (c2Mode == C2Mode::FRESH && m_c2.lossRatePct > ABR_C2_LOSS_PAUSE_PCT);
    m_c2LossTicks = c2Lossy ? m_c2LossTicks + 1 : 0;

    // Score-history streaks. Count THIS tick's sample — the freshness guard
    // above already returned if there was no fresh video row, so a skipped tick
    // FREEZES the streaks (never reaches here). Update BEFORE the call: the core
    // compares the streak to ABR_SCORE_STREAK_TICKS, so it reads the count
    // INCLUDING this tick — unlike notUsableTicks/c2LossTicks, whose core adds 1
    // to a "before" value. A low sample clears the high streak and vice-versa; a
    // deadband sample (neither low nor high) clears both.
    if (m_video.score < ABR_SCORE_DEC_MIN) {
        ++m_scoreLowStreak;
        m_scoreHighStreak = 0;
    } else if (m_video.score >= ABR_SCORE_UP_MIN) {
        ++m_scoreHighStreak;
        m_scoreLowStreak = 0;
    } else {
        m_scoreLowStreak = 0;
        m_scoreHighStreak = 0;
    }

    AbrCounters cnt;
    cnt.notUsableTicks  = ticksBefore;
    cnt.c2LossTicks     = c2LossTicksBefore;
    cnt.scoreLowStreak  = m_scoreLowStreak;
    cnt.scoreHighStreak = m_scoreHighStreak;

    // Ceiling memory (see ABR_CEILING_*). Resolve the LIVE cap from the PREVIOUS
    // tick's memory BEFORE the call, exactly as c2Mode is resolved before
    // m_lastComputeMs: the cap this tick's decision obeys is the one already on
    // file, not one this very decision might write. A memory older than the dwell
    // is dead — zero it here so the log and the cap agree, and so a stale entry
    // cannot resurrect itself.
    if (m_ceilingKbps > 0 && m_clock.elapsed() - m_ceilingSinceMs >= ABR_CEILING_DWELL_MS)
        m_ceilingKbps = 0;
    const int rampCapKbps = (m_ceilingKbps > 0)
                                ? static_cast<int>(m_ceilingKbps * ABR_CEILING_BACKOFF)
                                : 0;

    const AbrDecision d = decideBitrate(m_video, m_c2, c2Mode, m_bitrate, cnt,
                                        m_cameraCollapsed,
                                        sinceLastChangeMs, m_maxAbr, rampCapKbps);
    const int b = d.kbps;

    // ── state update ─────────────────────────────────────────────────────
    if (b == QOS_NOT_AVAILABLE) return d;   // -1: leave state alone

    if (b != m_bitrate) {
        const int from = m_bitrate;
        // Ceiling memory: a DECREASE from a level we were not already backing off
        // from records that level as the wall. The guard keeps a CHAIN of cuts
        // from overwriting the true top with each lower step (matches xbfirm
        // setNewBitrate: only record when the previous action was not a decrease).
        // An INCREASE past the recorded level means we climbed back over the wall
        // — clear it, the link recovered.
        if (b < from) {
            const bool wasBackingOff = (m_ceilingKbps > 0) && (from <= m_ceilingKbps);
            if (!wasBackingOff) {
                m_ceilingKbps = from;
                m_ceilingSinceMs = m_clock.elapsed();
            }
        } else if (m_ceilingKbps > 0 && b > m_ceilingKbps) {
            m_ceilingKbps = 0;
        }

        m_lastChangeMs = m_clock.elapsed();
        // Reset-after-fire: the streak that produced this change has done its
        // job. Clearing BOTH here means a slow x0.80 needs 3 NEW low samples
        // before repeating, and a fast loss-halve clears the low streak so a
        // x0.80 cannot stack on the same event next tick.
        m_scoreLowStreak  = 0;
        m_scoreHighStreak = 0;
    }
    m_bitrate = b;

    // ── Part B (§6c): stuck detection ────────────────────────────────────
    updateStuckObservation(m_video.queuingDelayMs, b);

    // The decision reads score, loss and trend; rtt+ (rtt - minRtt) is logged
    // for diagnosis only, and the router's queuingDelayMs is dropped entirely —
    // the two disagreed by orders of magnitude (2026-10-06) and only rtt+ is the
    // physical queuing delay. If a future change makes rtt+ drive a branch, this
    // line already carries it.
    qDebug().nospace() << "[ABR][" << m_iface << "] c2=" << c2ModeName(c2Mode)
                       << " score=" << QString::number(m_video.score, 'f', 1)
                       << " trend=" << m_video.trend
                       << " lowStreak=" << m_scoreLowStreak << " upStreak=" << m_scoreHighStreak
                       << " rtt+=" << QString::number(m_video.rttMs - m_video.minRttMs, 'f', 1)
                       << " loss=" << QString::number(m_video.lossRatePct, 'f', 3)
                       << " cap=" << rampCapKbps
                       << " -> " << b << "kbps";

    return d;
}

void XBAdaptiveBitrateStreaming::updateStuckObservation(double queuingDelayMs, int emittedBitrate)
{
    // PART B — a reset is DESTRUCTIVE (teardown + reconnect = frozen picture).
    // It is warranted only for a STUCK queue: congestion that persists AFTER
    // the bitrate has already been lowered. Plain congestion is fixed by
    // lowering and must never trigger a reset. See report §6c.
    //
    // Enter observation when either:
    //   (a) this interface just became primary (a switch toward us — the sender
    //       may still be pushing the old interface's high bitrate), or
    //   (b) overdrive: measured throughput is far below what we are asking for.
    // Both only START a watch; the reset decision is made after the window.
    // The ratio is a DIVISOR: we are overdriving when the link delivers less
    // than 1/ABR_OVERDRIVE_RATIO of the target (can't sustain ~62% of it).
    // Written as a multiply it was true in normal operation — load ≈ bitrate
    // is always < 1.6×bitrate — so the watch re-armed every tick forever.
    const bool overdrive = (m_bitrate > ABR_OVERDRIVE_MIN_BITRATE_KBPS)
                           && (m_video.throughputKbps > 0.0)
                           && (m_video.throughputKbps < m_bitrate / ABR_OVERDRIVE_RATIO);
    if (!m_observing && overdrive) {
        m_observing = true;
        m_obsQdelay0 = queuingDelayMs;
        m_obsTicks = 0;
        qInfo().nospace() << "[ABR][" << m_iface << "] observing (overdrive: thr="
                          << QString::number(m_video.throughputKbps, 'f', 0)
                          << "kbps < bitrate=" << m_bitrate << ")";
    }

    if (!m_observing) return;

    ++m_obsTicks;

    // Drained enough within the window → it was congestion, not a deadlock.
    const bool drained = (m_obsQdelay0 > 0.0)
                         && (queuingDelayMs <= m_obsQdelay0 * (1.0 - ABR_STUCK_DRAIN_DROP_PCT));
    if (drained) {
        qInfo().nospace() << "[ABR][" << m_iface << "] observation cleared (qdelay drained "
                          << m_obsQdelay0 << " -> " << queuingDelayMs << "ms)";
        m_observing = false;
        m_obsTicks = 0;
        return;
    }

    if (m_obsTicks < ABR_STUCK_OBSERVE_TICKS) return;

    // Window elapsed with qdelay still high and undrained → stuck.
    if (queuingDelayMs > ABR_STUCK_QDELAY_HIGH_MS) {
        // Reset is destructive; rate-limit so a genuinely stuck link emits at
        // most once per cooldown instead of re-firing every ~5 ticks. The
        // executor (CameraControl) owns the real reset policy — this is only a
        // guard against a signal storm from the detector.
        const qint64 sinceResetMs = m_clock.elapsed() - m_lastResetMs;
        if (m_lastResetMs == 0 || sinceResetMs >= ABR_RESET_COOLDOWN_MS) {
            qWarning().nospace() << "[ABR][" << m_iface
                                 << "] STUCK — qdelay " << queuingDelayMs
                                 << "ms undrained after " << m_obsTicks
                                 << " ticks at lowered bitrate → requestStreamReset";
            m_lastResetMs = m_clock.elapsed();
            emit requestStreamReset(m_iface, QStringLiteral("stuck"));
        }
    }
    m_observing = false;
    m_obsTicks = 0;
    Q_UNUSED(emittedBitrate);
}

void XBAdaptiveBitrateStreaming::handleRouterTick(const QString &primaryInterface)
{
    // ── Part B entry: did we just become primary? (§6c) ──────────────────
    const bool isPrimary = (primaryInterface == m_iface);
    if (isPrimary && !m_wasPrimary) {
        qInfo().nospace() << "[ABR][" << m_iface << "] became primary — begin watch";
        if (!m_observing) {
            m_observing = true;
            m_obsQdelay0 = m_video.queuingDelayMs;
            m_obsTicks = 0;
        }
    }
    m_wasPrimary = isPrimary;

    const AbrDecision d = processQoS();
    if (d.kbps == QOS_NOT_AVAILABLE) return;   // -1: cannot decide → do not emit

    // The full decision goes out EVERY computable tick, holds included: the QS
    // channel wants the verdict beside its inputs on each sample, not only when
    // the bitrate moves. bitrateProposed keeps its deadband below — a consumer
    // acting on the video only cares when the number changes.
    emit decisionMade(m_iface, d.kbps, static_cast<int>(m_lastC2Mode),
                      static_cast<int>(d.branch), static_cast<int>(d.reason), m_maxAbr);

    const int b = d.kbps;
    if (b == m_lastEmitted) return;       // deadband at the emit layer

    m_lastEmitted = b;
    emit bitrateProposed(m_iface, static_cast<unsigned int>(b));
}
