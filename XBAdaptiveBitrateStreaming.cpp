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
}

void XBAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    // Internal state reset only — NOT a socket reset. Mirrors the base
    // contract; the socket-reset path is a separate, unimplemented signal.
    m_bitrate = (bitrateKbps > 0) ? bitrateKbps : ABR_MIN_BITRATE_KBPS;
    m_notUsableTicks = 0;
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

int XBAdaptiveBitrateStreaming::processQoS()
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
        return QOS_NOT_AVAILABLE;
    }
    m_staleWarned = false;
    m_lastComputeMs = m_clock.elapsed();

    const C2Mode c2Mode = c2ModeForThisTick();

    // C2 declared present but never seen → warn once, after a grace period, so
    // a slow C2 subflow at startup is not mistaken for a misconfiguration.
    if (c2Mode == C2Mode::NEVER_SEEN && !m_c2Warned
        && m_clock.elapsed() > ABR_C2_GRACE_MS) {
        qWarning().nospace() << "[ABR][" << m_iface
                             << "] C2 declared configured but 601 never seen — blocking INCREASE";
        m_c2Warned = true;
    }

    const qint64 sinceLastChangeMs = m_clock.elapsed() - m_lastChangeMs;

    const int b = decideBitrate(m_video, m_c2, c2Mode, m_bitrate,
                                m_notUsableTicks, sinceLastChangeMs, m_maxAbr);

    // ── state update ─────────────────────────────────────────────────────
    if (b == QOS_NOT_AVAILABLE) return QOS_NOT_AVAILABLE;   // -1: leave state alone

    m_notUsableTicks = (b == QOS_PAUSE) ? m_notUsableTicks + 1 : 0;
    if (b != m_bitrate) m_lastChangeMs = m_clock.elapsed();
    m_bitrate = b;

    // ── Part B (§6c): stuck detection ────────────────────────────────────
    updateStuckObservation(m_video.queuingDelayMs, b);

    qDebug().nospace() << "[ABR][" << m_iface << "] c2=" << c2ModeName(c2Mode)
                       << " score=" << QString::number(m_video.score, 'f', 1)
                       << " trend=" << m_video.trend
                       << " qdelay=" << QString::number(m_video.queuingDelayMs, 'f', 1)
                       << " loss=" << QString::number(m_video.lossRatePct, 'f', 3)
                       << " -> " << b << "kbps";

    return b;
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
    const bool overdrive = (m_bitrate > ABR_MIN_BITRATE_KBPS)
                           && (m_video.throughputKbps > 0.0)
                           && (m_video.throughputKbps < m_bitrate * ABR_OVERDRIVE_RATIO);
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
        qWarning().nospace() << "[ABR][" << m_iface
                             << "] STUCK — qdelay " << queuingDelayMs
                             << "ms undrained after " << m_obsTicks
                             << " ticks at lowered bitrate → requestStreamReset";
        emit requestStreamReset(m_iface, QStringLiteral("stuck"));
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

    const int b = processQoS();
    if (b == QOS_NOT_AVAILABLE) return;   // -1: cannot decide → do not emit
    if (b == m_lastEmitted) return;       // deadband at the emit layer

    m_lastEmitted = b;
    emit bitrateProposed(m_iface, static_cast<unsigned int>(b));
}
