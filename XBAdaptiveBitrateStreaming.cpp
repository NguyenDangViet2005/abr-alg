#include "XBAdaptiveBitrateStreaming.h"
#include <QDebug>
#include <QtMath>

// Verbatim port of xbfirm's XBAdaptiveBitrateStreaming
// (aiCompressor/XBAdaptiveBitrateStreaming.cpp). The core — haveLostPackets,
// rttHigh, calculateJitter, checkSenderQueue, setNewBitrate,
// processTCPInfo, processTelemetryTCPInfo, updateStreamingStatus, reset — is
// copied as-is. Every deviation is a comment beginning with "PORT DEVIATION".

static const char *statusName(int s)
{
    switch (s) {
    case 0: return "Playing (Auto)";
    case 1: return "Optimizing";
    case 2: return "Poor Network";
    case 3: return "Buffering / Reconnecting";
    case 4: return "Loading";
    }
    return "?";
}

// === CONSTRUCTOR / DESTRUCTOR ===

XBAdaptiveBitrateStreaming::XBAdaptiveBitrateStreaming(const QString &iface,
                                                       bool isC2Configured,
                                                       QObject *parent)
    : IAdaptiveBitrateStreaming(parent)
    , m_maxAbr(ABR_DEFAULT_MAX_ABR_BITRATE)
    , m_iface(iface)
    , m_isC2Configured(isC2Configured)
{
    // xbfirm ctor: m_abrState.current_bitrate_kbps = 800; maxAbr = MAX_BITRATE_KBPS.
    m_abrState.current_bitrate_kbps = 800;
    m_problematicBitrateThreshold = m_maxAbr;

    // PORT DEVIATION (timing): xbfirm uses QDateTime::currentMSecsSinceEpoch()
    // for every elapsed measurement. The epoch is irrelevant and non-monotonic;
    // QElapsedTimer measures the same elapsed ms without NTP-jump risk.
    m_clock.start();

    m_networkDataTimer = new QTimer(this);
    m_networkDataTimer->setInterval(5000);
    m_networkDataTimer->setSingleShot(false);
    connect(m_networkDataTimer, &QTimer::timeout,
            this, &XBAdaptiveBitrateStreaming::onNetworkDataTimeout);

    m_statusBroadcastTimer = new QTimer(this);
    m_statusBroadcastTimer->setInterval(5000);
    m_statusBroadcastTimer->setSingleShot(false);
    connect(m_statusBroadcastTimer, &QTimer::timeout,
            this, &XBAdaptiveBitrateStreaming::onStatusBroadcastTimeout);

    qInfo().nospace() << "[ABR][" << m_iface << "] instance created (isC2Configured="
                      << m_isC2Configured << ", maxAbr=" << m_maxAbr << ")";
}

XBAdaptiveBitrateStreaming::~XBAdaptiveBitrateStreaming()
{
    if (m_networkDataTimer) { m_networkDataTimer->stop(); delete m_networkDataTimer; m_networkDataTimer = nullptr; }
    if (m_statusBroadcastTimer) { m_statusBroadcastTimer->stop(); delete m_statusBroadcastTimer; m_statusBroadcastTimer = nullptr; }
}

// === xbfirm PUBLIC API ===

void XBAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{
    // xbfirm setMaxAbrBitrate, verbatim.
    m_maxAbr = newMaxAbrBitrate;
    m_problematicBitrateThreshold = m_maxAbr;
    // Phase 01 (PORT DEVIATION): a new max invalidates any learned ceiling —
    // the ramp is free again up to the new max.
    m_ceilingKbps = m_maxAbr;
}

void XBAdaptiveBitrateStreaming::start()
{
    // xbfirm start(), verbatim. Resets to 1000 and starts the two 5s timers.
    if (m_isStarted) return;

    reset(1000);

    m_isStarted = true;
    m_lastNetworkDataTime = m_clock.elapsed();

    if (m_networkDataTimer) m_networkDataTimer->start();
    if (m_statusBroadcastTimer) m_statusBroadcastTimer->start();
}

void XBAdaptiveBitrateStreaming::stop()
{
    // xbfirm stop(), verbatim.
    if (!m_isStarted) return;

    m_isStarted = false;
    if (m_networkDataTimer && m_networkDataTimer->isActive()) m_networkDataTimer->stop();
    if (m_statusBroadcastTimer && m_statusBroadcastTimer->isActive()) m_statusBroadcastTimer->stop();

    m_lastNetworkDataTime = 0;
    m_rttHistory.clear();
    m_consecutivePoorConditions = 0;
}

void XBAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    // xbfirm reset(), verbatim. Note it does NOT reset
    // m_problematicBitrateThreshold (commented out in xbfirm) and restores the
    // advisor gains to 1.0.
    _cameraStatus = true;
    m_abrState.current_bitrate_kbps = static_cast<unsigned int>(bitrateKbps > 0 ? bitrateKbps : 0);
    m_lastAbrAction = ABR_NONE;
    m_lastQualityChangeTime = 0;
    m_avgSiocoutq = 0.0;
    m_rttHistory.clear();

    m_currentStatus = LOADING;
    m_previousStatus = LOADING;
    m_consecutivePoorConditions = 0;
    m_lastStatusChangeTime = 0;

    m_lastNetworkDataTime = 0;
    _lastTcpiSegsout = 0;
    _lastTelemetrySegsout = 0;

    m_alpha = 1.0;
    m_gainUp = 1.0;
    m_gainDown = 1.0;

    // Phase 01 (PORT DEVIATION): a fresh session has no learned ceiling yet, so
    // the ramp is free up to maxAbr until an overshoot sets one. last-good is
    // unknown until the first step-up records it.
    m_ceilingKbps = m_maxAbr;
    m_lastGoodKbps = 0;
    m_scoreAtStepUp = 0.0;
    m_trendAtStepUp = 0;
    m_ticksSinceStepUp = 0;
    m_lastStepUpMs = 0;
}

// === ROUTER WIRING (not xbfirm) ===

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
        // A row whose fields could not be parsed carries plausible zeros. Do not
        // store it — treat as silence.
        qWarning().nospace() << "[ABR][" << m_iface << "] row parseFailed — not stored";
        return;
    }

    m_lastNetworkDataTime = m_clock.elapsed();

    if (row.connType == ConnType::VIDEO) {
        m_video = row;

        // Phase 01 (PORT DEVIATION): age the causal window. setNewBitrate()
        // resets this to 0 on an increase, so at the moment a loss is
        // classified it holds the ticks elapsed since our last step-up.
        m_ticksSinceStepUp++;

        // VIDEO row -> xbfirm's 11-arg processTCPInfo. The router's raw block
        // (fields 7..16) is EXACTLY that argument list, positionally:
        //   rtt µs, deliveryRate B/s, tcpiLoss count, tcpiSegsout count,
        //   sndCwnd, sndMss, lastAckRecv, sockOutq bytes, sndBuf bytes, minRtt µs.
        // So this is a 1:1 call — no unit conversion here. xbfirm's own 7-arg
        // overload does the µs->ms / B/s->kbps / count->percent conversion, and
        // it also carries the `if (!_cameraStatus) return;` gate, which is why
        // the gate is NOT duplicated above: it lives inside the 7-arg stage
        // exactly as in xbfirm. (The const-ref core stays ungated, as in xbfirm.)
        processTCPInfo(row.rttUs,             // rtt        (µs)
                       row.deliveryRateBps,   // deliveryRate (B/s)
                       row.rttUs,             // tcpiRtt    (µs, dropped by 11-arg)
                       row.lost,              // tcpiLoss   (count)
                       row.segsOut,           // tcpiSegsout(count)
                       row.sndCwnd,           // sndCwnd    (dropped by 11-arg)
                       row.sndMss,            // sndMss     (dropped by 11-arg)
                       row.lastAckRecv,       // lastAckRecv(dropped by 11-arg)
                       row.sockOutq,          // sockOutq   (bytes)
                       row.sockSndBuf,        // sockSndBuf (bytes, router sends 0)
                       row.minRttUs);         // tcpiMinRtt (µs)
    } else if (row.connType == ConnType::C2) {
        m_c2 = row;
        // The C2 subflow exists -> xbfirm's _haveTelemetry. Without this,
        // rttHigh never fires and the telemetry-loss branch reads 0.
        _haveTelemetry = true;

        // C2 row -> xbfirm's telemetry slot (processTelemetryTCPInfo). Same raw
        // units as the video call: the router's 601 carries the real counters, so
        // xbfirm's own delta(tcpiLoss)/delta(tcpiSegsout) derives the telemetry
        // loss percent with no synthetic counter. This is the 1:1 argument list
        // of xbfirm's telemetry caller.
        processTelemetryTCPInfo(row.rttUs,            // rtt        (µs)
                                row.deliveryRateBps,  // deliveryRate (B/s)
                                row.rttUs,            // tcpiRtt    (µs, unused)
                                row.lost,             // tcpiLoss   (count)
                                row.segsOut,          // tcpiSegsout(count)
                                row.sndCwnd,          // sndCwnd    (unused)
                                row.sndMss,           // sndMss     (unused)
                                row.lastAckRecv,      // lastAckRecv(unused)
                                row.sockOutq,         // sockOutq   (unused)
                                row.sockSndBuf,       // sockSndBuf (unused)
                                row.minRttUs);        // tcpiMinRtt (µs)
    }
}

void XBAdaptiveBitrateStreaming::handleRouterTick(const QString &primaryInterface)
{
    // xbfirm has no per-tick hook — its algorithm is edge-triggered from
    // processTCPInfo. This exists so the QS debug channel gets a per-tick
    // verdict; it does NOT feed the algorithm.
    //
    // branch/reason are VESTIGIAL: xbfirm has no branch concept, so the value is
    // always HOLD(0)/NONE(0). The wire keeps both columns (frozen — the
    // dashboard decodes positionally); only the values are pinned to 0. Emitted
    // as plain ints (no include of the wire enum) so this class stays decoupled.
    Q_UNUSED(primaryInterface);
    emit decisionMade(m_iface, static_cast<int>(m_abrState.current_bitrate_kbps),
                      0, /*branch=HOLD*/ 0, /*reason=NONE*/ 0,
                      static_cast<int>(m_maxAbr));
}

void XBAdaptiveBitrateStreaming::handleCameraReportedBitrate(int achievedKbps)
{
    // PORT DEVIATION: xbfirm has NO camera-collapse input. Stored for logging
    // only; it does NOT gate the algorithm (that would be non-xbfirm logic).
    if (achievedKbps <= 0) return;
    m_lastAchievedKbps = achievedKbps;
    qInfo().nospace() << "[ABR][" << m_iface << "] camera reported achieved="
                      << achievedKbps << "kbps (asked="
                      << m_abrState.current_bitrate_kbps << "kbps)";
}

// === xbfirm CORE — processTCPInfo ===

void XBAdaptiveBitrateStreaming::processTCPInfo(const tcpInfo &tcpi)
{
    // xbfirm processTCPInfo(const tcpInfo&), verbatim.
    m_lastNetworkDataTime = m_clock.elapsed();

    bool bitrateChanged = false;

    if (haveLostPackets(tcpi)) {
        bitrateChanged = true;
    } else if (rttHigh(tcpi)) {
        bitrateChanged = true;
    } else {
        unsigned int oldBitrate = m_abrState.current_bitrate_kbps;
        checkSenderQueue(tcpi);
        bitrateChanged = (oldBitrate != m_abrState.current_bitrate_kbps);
    }

    updateStreamingStatus(tcpi, bitrateChanged);
}

void XBAdaptiveBitrateStreaming::processTCPInfo(quint32 rtt, quint32 min_rtt,
                                                double throughput, double loss_rate,
                                                quint32 siocoutq, quint32 so_sndbuf,
                                                int tcpiSegsout)
{
    // xbfirm processTCPInfo(quint32,...), verbatim. Converts raw units to
    // tcpInfo's (ms / kbps / percent) and gates on _cameraStatus before
    // forwarding to the core. NOTE the gate lives HERE, not at the router-row
    // entry: xbfirm puts it in this overload, and the 11-arg caller above simply
    // forwards through it.
    int deltaSegsout = tcpiSegsout - _lastTcpiSegsout;
    _lastTcpiSegsout = tcpiSegsout;

    tcpInfo tcpi;
    tcpi.rtt = rtt / 1000;
    tcpi.min_rtt = min_rtt / 1000;
    tcpi.throughput = (throughput * 8) / 1000.0;
    // loss_rate arrives as a lost-packet COUNT; dividing by the packets sent in
    // this interval yields the percent. This is why the router sends the raw
    // counters rather than a percent — the same arithmetic xbfirm runs.
    tcpi.loss_rate = (deltaSegsout > 0) ? (loss_rate / deltaSegsout * 100.0) : 0.0;
    tcpi.siocoutq = siocoutq;
    tcpi.so_sndbuf = so_sndbuf;
    if (!_cameraStatus) return;
    this->processTCPInfo(tcpi);
}

void XBAdaptiveBitrateStreaming::processTCPInfo(double rtt, double deliveryRate,
                                                double tcpiRtt, int tcpiLoss,
                                                int tcpiSegsout, int tcpiSndCwnd,
                                                int tcpiSndMss, int tcpiLastAckRecv,
                                                int sockOutq, int sockSndBuf,
                                                int tcpiMinRtt)
{
    // xbfirm processTCPInfo(double,...), verbatim: forwards to the 7-arg
    // overload, DROPPING tcpiSndCwnd/tcpiSndMss/tcpiLastAckRecv and the
    // duplicated tcpiRtt. Kept as a distinct overload so the port consumes the
    // router's 22-field row in xbfirm's own wire order.
    Q_UNUSED(tcpiRtt);
    Q_UNUSED(tcpiSndCwnd);
    Q_UNUSED(tcpiSndMss);
    Q_UNUSED(tcpiLastAckRecv);
    this->processTCPInfo(static_cast<quint32>(rtt),
                         static_cast<quint32>(tcpiMinRtt),
                         deliveryRate, tcpiLoss, static_cast<quint32>(sockOutq),
                         static_cast<quint32>(sockSndBuf), tcpiSegsout);
}

void XBAdaptiveBitrateStreaming::processTelemetryTCPInfo(double rtt, double deliveryRate,
                                                         double tcpiRtt, int tcpiLoss,
                                                         int tcpiSegsout, int tcpiSndCwnd,
                                                         int tcpiSndMss, int tcpiLastAckRecv,
                                                         int sockOutq, int sockSndBuf,
                                                         int tcpiMinRtt)
{
    // xbfirm processTelemetryTCPInfo(...), verbatim. Only rtt/minRtt/loss are
    // read; the rest of the signature is kept for parity with xbfirm's caller.
    Q_UNUSED(deliveryRate);
    Q_UNUSED(tcpiRtt);
    Q_UNUSED(tcpiSndCwnd);
    Q_UNUSED(tcpiSndMss);
    Q_UNUSED(tcpiLastAckRecv);
    Q_UNUSED(sockOutq);
    Q_UNUSED(sockSndBuf);

    if (!_haveTelemetry) return;

    int deltaSegsout = tcpiSegsout - _lastTelemetrySegsout;
    _lastTelemetrySegsout = tcpiSegsout;
    const double lossRate = (deltaSegsout > 0)
                                ? (static_cast<double>(tcpiLoss) / deltaSegsout * 100.0)
                                : 0.0;

    m_telemetryLossRate = lossRate;
    m_telemetryRtt      = static_cast<quint32>(rtt / 1000);
    m_telemetryMinRtt   = static_cast<quint32>(tcpiMinRtt / 1000);

    if (lossRate > 1.0) {
        if (m_abrState.current_bitrate_kbps > MIN_BITRATE_KBPS
            && m_abrState.current_bitrate_kbps < m_problematicBitrateThreshold) {
            m_problematicBitrateThreshold = m_abrState.current_bitrate_kbps;
            qWarning().nospace() << "[ABR][" << m_iface
                                 << "] Kick failed: recorded problematic threshold ="
                                 << m_problematicBitrateThreshold << "kbps";
        }
        _cameraStatus = false;
        m_telemetryStableSince = 0;
        m_abrState.current_bitrate_kbps = MIN_BITRATE_KBPS;
        emit bitrateProposed(m_iface, 0);
        return;
    } else {
        if (!_cameraStatus) {
            const qint64 now = m_clock.elapsed();

            if (m_telemetryStableSince == 0) m_telemetryStableSince = now;

            if (now - m_telemetryStableSince >= static_cast<qint64>(TELEMETRY_STABLE_TIMEOUT_MS)) {
                _cameraStatus = true;
                m_telemetryStableSince = 0;

                if (m_abrState.current_bitrate_kbps <= MIN_BITRATE_KBPS) {
                    // PORT DEVIATION (Phase 01, M3): this kick may EXCEED
                    // m_ceilingKbps, by design. It is a post-outage probe — the
                    // link was just proven clean for TELEMETRY_STABLE_TIMEOUT_MS
                    // after a telemetry-loss collapse, which is independent
                    // evidence the environment changed. Clamping it to a ceiling
                    // learned before that outage would defeat the probe. This is
                    // the ONE documented exception to "the ramp never exceeds the
                    // ceiling"; every steady-state ramp is still gated.
                    unsigned int kickBitrate;
                    if (m_problematicBitrateThreshold < m_maxAbr) {
                        kickBitrate = static_cast<unsigned int>(m_problematicBitrateThreshold * 0.9);
                        kickBitrate = qBound(MIN_BITRATE_KBPS + BITRATE_STEP_KBPS, kickBitrate, m_maxAbr);
                        qWarning().nospace() << "[ABR][" << m_iface
                                             << "] ABR Kick (smart): MIN ->" << kickBitrate
                                             << "kbps (threshold:" << m_problematicBitrateThreshold << "kbps)";
                    } else {
                        kickBitrate = qMin(TELEMETRY_KICK_BITRATE_KBPS, m_maxAbr);
                        qWarning().nospace() << "[ABR][" << m_iface
                                             << "] ABR Kick (probe): MIN ->" << kickBitrate << "kbps";
                    }
                    setNewBitrate(kickBitrate);
                } else {
                    qInfo().nospace() << "[ABR][" << m_iface << "] ABR recovery: bitrate already at "
                                      << m_abrState.current_bitrate_kbps << "kbps, no kick needed";
                }
            }
        }
    }
}

// === xbfirm CORE — decision branches ===

bool XBAdaptiveBitrateStreaming::haveLostPackets(const tcpInfo &tcpi)
{
    // xbfirm haveLostPackets, verbatim, except the self-congestion branch (see
    // the PORT DEVIATION below). Trigger is unchanged: video loss > 0.8%.
    if (tcpi.loss_rate <= 0.8) return false;

    if (m_telemetryLossRate > 0.3) {
        // C2 (the independent link probe) is ALSO lossy -> the link itself is
        // weak. xbfirm, verbatim: drop to MIN.
        qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                             << "% + Telemetry loss: " << m_telemetryLossRate
                             << "% -> Network degraded, dropping to MIN";
        setNewBitrate(MIN_BITRATE_KBPS);
    } else if (isOvershoot()) {
        // PORT DEVIATION (Phase 01): telemetry is CLEAN and the break landed
        // right after our own step-up -> this is our overshoot, not a weak
        // link. Fall back to last-good and remember it as the ceiling, so the
        // ramp holds there instead of halving-and-climbing again (the
        // sawtooth). xbfirm had no such memory; it halved unconditionally.
        // M4: a loss event must never RAISE the bitrate. If another decrease
        // already fired inside the window (e.g. rttHigh), current < last-good,
        // and falling back "up" would undo it and re-arm the classifier. Take
        // the lower of the two. The ceiling is floored at MIN+STEP so a
        // recovery-from-MIN step-up can never pin it to a permanent stall.
        const unsigned int target = qMin(m_lastGoodKbps, m_abrState.current_bitrate_kbps);
        m_ceilingKbps = qMax(target, MIN_BITRATE_KBPS + BITRATE_STEP_KBPS);
        qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                             << "% + Telemetry OK (" << m_telemetryLossRate
                             << "%), " << m_ticksSinceStepUp << " tick(s) after step-up -> overshoot;"
                             << " back to " << target << "kbps, ceiling=" << m_ceilingKbps << "kbps";
        setNewBitrate(target);
    } else {
        // Telemetry clean but the break did NOT follow a recent step-up: it is
        // unrelated to our ramp. Keep xbfirm's self-congestion response.
        unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * 0.50);
        qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                             << "% but Telemetry OK (" << m_telemetryLossRate
                             << "%) -> Self-congestion, reducing 50% to " << new_bitrate << "kbps";
        setNewBitrate(new_bitrate);
    }
    m_rttHistory.clear();
    return true;
}

bool XBAdaptiveBitrateStreaming::isOvershoot() const
{
    // PORT DEVIATION (Phase 01): the causal test. Both "our overshoot" and
    // "weak link" present as a low/falling video score, so a LEVEL comparison
    // cannot tell them apart. The difference is TIMING: an overshoot break
    // starts right after our own step-up, from a link that was healthy when we
    // pushed. So the test is relative to the step-up, never an absolute score.
    //
    // Requires: (a) the last step-up recorded a healthy score AND a not-already
    // falling trend, (b) the break is within the window of that step-up in BOTH
    // tick count and wall-clock (so a data gap cannot freeze the counter and
    // make an old step-up look recent), and (c) there IS a recorded last-good
    // to fall back to (a step-up actually happened).
    const qint64 sinceStepUpMs = m_clock.elapsed() - m_lastStepUpMs;
    return m_lastGoodKbps > 0
        && m_ticksSinceStepUp <= OVERSHOOT_WINDOW_TICKS
        && sinceStepUpMs <= OVERSHOOT_WINDOW_MS
        && m_scoreAtStepUp >= OVERSHOOT_MIN_SCORE_AT_STEPUP
        && m_trendAtStepUp <= OVERSHOOT_MAX_TREND_AT_STEPUP;
}

bool XBAdaptiveBitrateStreaming::rttHigh(const tcpInfo &tcpi)
{
    // xbfirm rttHigh, verbatim. NOTE: reads the TELEMETRY (C2) rtt, not the
    // video socket's — tcpi is unused. The threshold is hardcoded 500 in xbfirm
    // (RTT_HIGH_THRESHOLD_MS is declared but not used there either).
    Q_UNUSED(tcpi);
    quint32 queuing_delay = m_telemetryRtt - m_telemetryMinRtt;
    if (queuing_delay <= 500) return false;

    qWarning().nospace() << "[ABR][" << m_iface << "] High Telemetry RTT (Queuing Delay > 500ms): "
                         << queuing_delay << "ms (RTT:" << m_telemetryRtt
                         << "ms, MinRTT:" << m_telemetryMinRtt << "ms)";
    unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * 0.80);
    setNewBitrate(new_bitrate);
    m_rttHistory.clear();
    return true;
}

double XBAdaptiveBitrateStreaming::calculateJitter()
{
    // xbfirm calculateJitter, verbatim.
    if (m_rttHistory.size() < 2) return 0.0;

    double sum = 0.0;
    for (const auto &val : m_rttHistory) { sum += val; }
    const double mean = sum / m_rttHistory.size();

    double sq_sum = 0.0;
    for (const auto &val : m_rttHistory) { sq_sum += qPow(val - mean, 2); }
    return qSqrt(sq_sum / m_rttHistory.size());
}

void XBAdaptiveBitrateStreaming::checkSenderQueue(const tcpInfo &tcpi)
{
    // xbfirm checkSenderQueue, verbatim. (The stray unused
    // `bool testNetworkDelay = ...` in xbfirm is dropped — it is dead.)
    m_rttHistory.append(tcpi.rtt);
    if (m_rttHistory.size() > RTT_HISTORY_SIZE) m_rttHistory.removeFirst();

    const double jitter = calculateJitter();

    constexpr double EMA_ALPHA = 0.5;
    m_avgSiocoutq = (EMA_ALPHA * tcpi.siocoutq) + ((1.0 - EMA_ALPHA) * m_avgSiocoutq);

    const unsigned int current_throughput_Bps = static_cast<unsigned int>((tcpi.throughput * 1000) / 8);

    if (current_throughput_Bps == 0) {
        qWarning().nospace() << "[ABR][" << m_iface << "] Throughput is 0! Dropping to MIN";
        setNewBitrate(MIN_BITRATE_KBPS);
        return;
    }

    const double queuing_delay_ms = (m_avgSiocoutq / static_cast<double>(current_throughput_Bps)) * 1000.0;

    const double increase_q_delay_threshold =
        (m_lastAbrAction == ABR_DECREASE) ? QUEUE_DELAY_LOW_AFTER_DECREASE_MS : QUEUE_DELAY_LOW_THRESHOLD_MS;
    const double increase_rtt_delay_threshold =
        (m_lastAbrAction == ABR_DECREASE) ? RTT_STABLE_AFTER_DECREASE_MS : RTT_STABLE_THRESHOLD_MS;

    if (queuing_delay_ms > QUEUE_DELAY_HIGH_THRESHOLD_MS) {
        qWarning().nospace() << "[ABR][" << m_iface << "] Queued time >"
                             << QUEUE_DELAY_HIGH_THRESHOLD_MS << "ms";
        unsigned int decreased_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * m_gainDown);
        setNewBitrate(decreased_bitrate);
        m_rttHistory.clear();
    } else if (queuing_delay_ms < increase_q_delay_threshold) {
        qWarning().nospace() << "[ABR][" << m_iface << "] Queued time <" << queuing_delay_ms << "ms";
        const quint32 network_delay = tcpi.rtt - tcpi.min_rtt;
        if (network_delay < increase_rtt_delay_threshold) {
            qWarning().nospace() << "[ABR][" << m_iface << "] Network delay <" << network_delay << "ms";
            if (jitter < JITTER_STABLE_THRESHOLD_MS) {
                qWarning().nospace() << "[ABR][" << m_iface << "] RTT stable (" << network_delay
                                     << "ms < " << increase_rtt_delay_threshold << "ms) & Jitter stable ("
                                     << jitter << "ms < " << JITTER_STABLE_THRESHOLD_MS << "ms)";
                const qint64 now = m_clock.elapsed();
                const qint64 time_since_last_change = now - m_lastQualityChangeTime;
                qWarning().nospace() << "[ABR][" << m_iface << "] Cooldown check: "
                                     << time_since_last_change << "ms since last change (need >= "
                                     << BITRATE_INCREASE_COOLDOWN_MS << "ms)";
                if (time_since_last_change >= BITRATE_INCREASE_COOLDOWN_MS) {
                    // Phase 01 (PORT DEVIATION): never ramp above the learned
                    // ceiling. xbfirm had no ceiling, so this is what stops the
                    // ramp-forever behaviour once an overshoot has set the
                    // limit. While holding, m_lastQualityChangeTime does not
                    // advance, so this warning repeats ~1 Hz — same cadence as
                    // the other qWarnings in this loop, accepted.
                    const unsigned int next_bitrate = m_abrState.current_bitrate_kbps
                                                    + static_cast<unsigned int>(m_gainUp * BITRATE_STEP_KBPS);
                    if (next_bitrate > m_ceilingKbps) {
                        qWarning().nospace() << "[ABR][" << m_iface << "] At ceiling "
                                             << m_ceilingKbps << "kbps — holding (no ramp)";
                    } else {
                        setNewBitrate(next_bitrate);
                    }
                }
            }
        }
    } else {
        qWarning().nospace() << "[ABR][" << m_iface << "] Another condition: Queued time is moderate ("
                             << increase_q_delay_threshold << " < " << queuing_delay_ms << "ms) < "
                             << QUEUE_DELAY_HIGH_THRESHOLD_MS << "ms). Holding bitrate.";
    }
}

void XBAdaptiveBitrateStreaming::setNewBitrate(unsigned int new_bitrate_kbps)
{
    // xbfirm setNewBitrate, with the Phase 01 deviations marked below.
    // `emit bitrateChanged(adjusted_bitrate)` is renamed to
    // bitrateProposed(m_iface, ...).
    double adjusted_bitrate_raw = new_bitrate_kbps * m_alpha;
    adjusted_bitrate_raw = qBound(MIN_BITRATE_KBPS,
                                  static_cast<unsigned int>(adjusted_bitrate_raw),
                                  m_maxAbr);

    const double diff_raw = (adjusted_bitrate_raw > m_abrState.current_bitrate_kbps)
                                ? (adjusted_bitrate_raw - m_abrState.current_bitrate_kbps)
                                : (m_abrState.current_bitrate_kbps - adjusted_bitrate_raw);
    if (diff_raw < MIN_BITRATE_CHANGE_THRESHOLD) return;

    const unsigned int adjusted_bitrate = static_cast<unsigned int>(adjusted_bitrate_raw);

    qInfo().nospace() << "[ABR][" << m_iface << "] BITRATE CHANGE: old="
                      << m_abrState.current_bitrate_kbps << "kbps -> new=" << adjusted_bitrate << "kbps";

    if (adjusted_bitrate < m_abrState.current_bitrate_kbps) {
        if (m_lastAbrAction == ABR_INCREASE || m_lastAbrAction == ABR_NONE) {
            m_problematicBitrateThreshold = m_abrState.current_bitrate_kbps;
        }
        m_lastAbrAction = ABR_DECREASE;
    } else {
        // PORT DEVIATION (Phase 01): record the value we are leaving as
        // last-good, and snapshot the video score at the instant of the
        // step-up — the causal pair isOvershoot() reads. Resetting
        // m_ticksSinceStepUp / m_lastStepUpMs here anchors the window to THIS
        // ramp.
        //
        // The MIN floor on last-good is a SAFETY, not bookkeeping: the telemetry
        // ABR kick ramps up from MIN after an outage, and if a break then lands
        // on the very next row the ceiling would be pinned to MIN(30) — a
        // permanent stall, since the ramp gate would never let it climb back.
        // Recording a last-good at the floor is meaningless (the link never
        // proved it could carry 30), so keep the previous, higher value instead.
        if (m_abrState.current_bitrate_kbps > MIN_BITRATE_KBPS) {
            m_lastGoodKbps = m_abrState.current_bitrate_kbps;
        }
        m_scoreAtStepUp = m_video.score;
        m_trendAtStepUp = m_video.trend;
        m_ticksSinceStepUp = 0;
        m_lastStepUpMs = m_clock.elapsed();
        // PORT DEVIATION (Phase 01, H2): xbfirm set ABR_INCREASE ONLY inside
        // `else if (adjusted >= m_problematicBitrateThreshold)`, so during a
        // recovery ramp below the recorded threshold the action stayed
        // ABR_DECREASE — which made checkSenderQueue use the STRICTER
        // *AFTER_DECREASE gates (q<20ms / rtt<20ms) and let
        // updateStreamingStatus still reach POOR_NETWORK. We set it
        // unconditionally because a durable ceiling means the ramp may never
        // re-reach the threshold, which would freeze the action at DECREASE
        // forever. Deliberate, not accidental: it LOOSENS the post-decrease
        // increase gates (normal q<100ms / rtt<50ms) and drops the POOR_NETWORK
        // UX state during recovery.
        m_lastAbrAction = ABR_INCREASE;
    }
    // NOTE (Phase 01): xbfirm's `else if (adjusted >= m_problematicBitrateThreshold)
    // { m_problematicBitrateThreshold = m_maxAbr; }` is GONE. It never gated the
    // ramp (only qBound(...) to maxAbr did), so it was NOT the ramp-forever
    // defect — m_ceilingKbps is the fix for that. What the reset actually did
    // was re-arm the telemetry kick: pre-ceiling, a ramp that re-reached the
    // peak reset the threshold to maxAbr, so a later recovery used the
    // conservative PROBE kick (500kbps). Without it the threshold keeps the
    // last decrease point, so recovery uses the aggressive SMART kick
    // (0.9x threshold). Accepted: the smart kick is the better guess when a
    // ceiling already caps the steady state.

    m_abrState.current_bitrate_kbps = adjusted_bitrate;
    m_lastQualityChangeTime = m_clock.elapsed();

    emit bitrateProposed(m_iface, adjusted_bitrate);
}

// === xbfirm CORE — UX status ===

void XBAdaptiveBitrateStreaming::updateStreamingStatus(const tcpInfo &tcpi, bool bitrateChanged)
{
    // xbfirm updateStreamingStatus, verbatim (QDateTime -> m_clock.elapsed()).
    StreamingQualityStatus newStatus = m_currentStatus;
    const qint64 now = m_clock.elapsed();

    if (tcpi.throughput == 0.0 || tcpi.loss_rate > 5.0) {
        newStatus = BUFFERING;
        m_consecutivePoorConditions++;
    } else if (m_previousStatus == BUFFERING && (now - m_lastStatusChangeTime) < 3000) {
        newStatus = LOADING;
        m_consecutivePoorConditions = 0;
    } else if (m_abrState.current_bitrate_kbps <= (MIN_BITRATE_KBPS + BITRATE_STEP_KBPS)
               || (m_lastAbrAction == ABR_DECREASE && m_consecutivePoorConditions >= 2)) {
        newStatus = POOR_NETWORK;
        if (m_lastAbrAction == ABR_DECREASE) m_consecutivePoorConditions++;
    } else if (bitrateChanged || (now - m_lastQualityChangeTime) < 3000) {
        newStatus = OPTIMIZING;
        if (m_lastAbrAction == ABR_INCREASE) m_consecutivePoorConditions = 0;
    } else {
        newStatus = PLAYING_AUTO;
        m_consecutivePoorConditions = 0;
    }

    if (newStatus != m_currentStatus) {
        m_previousStatus = m_currentStatus;
        m_currentStatus = newStatus;
        m_lastStatusChangeTime = now;
        qInfo().nospace() << "[ABR][" << m_iface << "] UX status -> " << statusName(newStatus);
        emit onStatus(static_cast<int>(newStatus));
    }
}

// === xbfirm TIMERS ===

void XBAdaptiveBitrateStreaming::onNetworkDataTimeout()
{
    // PORT DEVIATION: xbfirm uses this to REQUEST connection stats over a socket
    // (emit requestStartConnectionStats) when its transport goes quiet. The
    // router PUSHES at 1 Hz and this service owns no such socket, so the timer
    // is repurposed to WARN on router silence — the same "inputs went stale"
    // signal, without inventing a request path.
    if (!m_isStarted) return;
    const qint64 timeSinceLastData = m_clock.elapsed() - m_lastNetworkDataTime;
    if (timeSinceLastData >= 5000) {
        qWarning().nospace() << "[ABR][" << m_iface << "] no router row for "
                             << timeSinceLastData << "ms — ABR is open-loop on stale inputs";
    }
}

void XBAdaptiveBitrateStreaming::onStatusBroadcastTimeout()
{
    // xbfirm onStatusBroadcastTimeout, verbatim: re-broadcast the current UX
    // status every 5s so a consumer never misses a transition.
    if (!m_isStarted) return;
    emit onStatus(static_cast<int>(m_currentStatus));
}
