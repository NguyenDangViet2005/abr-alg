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

    // Phase 03: a fresh session has no learned ceiling, so the strength anchor is
    // meaningless — clear it (baseline, staleness, debounce) alongside the
    // ceiling it belongs to.
    m_networkRef.reset();

    // Phase 07: a fresh session has no settle window, no zero-throughput run,
    // and no telemetry-lossy run.
    m_settleUntil = 0;
    m_zeroThroughputTicks = 0;
    m_telemetryLossyTicks = 0;

    // Phase 08 (A): a fresh session has no loss history to average over.
    m_lossWindowLost.clear();
    m_lossWindowSegs.clear();
    m_videoLossThisTick = false;

    // Phase 08 (B): no overshoot episode is in flight on a fresh session.
    m_pendingCeiling = 0;
    m_overshootConfirmTicks = 0;

    // Phase 08 (C): no hold clock, no in-flight fallback probe, backoff at base.
    m_ceilingHoldSince = 0;
    m_ceilingReprobeBackoffMs = CEILING_REPROBE_BASE_MS;
    m_reprobeInFlight = false;
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
        // PORT DEVIATION (Phase 01): xbfirm sets _haveTelemetry from the
        // telemetry SOURCE's connection state via handleSerialStatus, NOT per
        // row. This port has no serial link, so a C2 row stands in for "the
        // telemetry source is connected". With no C2 source configured the flag
        // stays at its default `false` and the telemetry slot is inert — the
        // state the user asked for while telemetry is not yet wired.
        handleSerialStatus(true);

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
    // ceilingKbps: 0 until an overshoot teaches one, else the learned value.
    // m_ceilingKbps is initialised to m_maxAbr (ramp is free), which is NOT a
    // "found" ceiling — report 0 so the dashboard draws no line and the wire
    // says "not yet found" rather than a 2000kbps ceiling that means nothing.
    const int ceiling = (m_ceilingKbps < m_maxAbr) ? static_cast<int>(m_ceilingKbps) : 0;
    emit decisionMade(m_iface, static_cast<int>(m_abrState.current_bitrate_kbps),
                      0, /*branch=HOLD*/ 0, /*reason=NONE*/ 0,
                      static_cast<int>(m_maxAbr), ceiling);
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

void XBAdaptiveBitrateStreaming::handleNetworkStrength(int strength)
{
    // Radio strength for THIS interface (603), routed by name from the factory.
    // The ref owns all interpretation; this slot is just the push point.
    m_networkRef.setStrength(strength);
}

void XBAdaptiveBitrateStreaming::handleSerialStatus(bool isConnected)
{
    // xbfirm handleSerialStatus, verbatim (the commented-out qWarning is
    // xbfirm's own). The telemetry source's connection state IS _haveTelemetry;
    // losing it while _cameraStatus is pinned off resets the video path.
    if (_haveTelemetry != isConnected) {
        _haveTelemetry = isConnected;
        if (!_haveTelemetry && !_cameraStatus) {
            _cameraStatus = true;
            reset(500);
        }
    }
}

// === xbfirm CORE — processTCPInfo ===

void XBAdaptiveBitrateStreaming::processTCPInfo(const tcpInfo &tcpi)
{
    // xbfirm processTCPInfo(const tcpInfo&), verbatim.
    m_lastNetworkDataTime = m_clock.elapsed();

    bool bitrateChanged = false;

    // PORT DEVIATION (Phase 07): settle window. After an overshoot we HOLD and
    // ACCEPT the trailing loss for OVERSHOOT_SETTLE_MS — that loss is the queue
    // draining, not a new break. Acting on it would decrease below the ceiling
    // we just learned (and re-arm the classifier). Still update the UX status.
    if (m_settleUntil != 0 && m_clock.elapsed() < m_settleUntil) {
        updateStreamingStatus(tcpi, false);
        return;
    }
    m_settleUntil = 0;   // expired (or never set)

    // Phase 08 (B): track whether this tick BROKE (loss or bufferbloat). An
    // overshoot episode stays open only while the breaks keep coming; a tick that
    // breaks neither way closes it, so a staged ceiling is discarded unless the
    // very next tick confirms. (Note: a break that is NOT an overshoot — a
    // telemetry-lossy weak link, say — leaves a staged candidate in place; the
    // following overshoot break then confirms it. Rare and still a reasonable
    // ceiling, so it is accepted rather than special-cased.)
    const bool broke = haveLostPackets(tcpi) || rttHigh(tcpi);
    bitrateChanged = broke;
    if (!broke) {
        // No break this tick -> the overshoot episode (if any) did not repeat.
        m_pendingCeiling = 0;
        m_overshootConfirmTicks = 0;
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
    // Phase 08 (A): the FIRST video row after reset() (or ever) has
    // _lastTcpiSegsout == 0, so the delta would be the connection's FULL
    // cumulative segsOut. Harmless before A (it yielded ~0%), but the window would
    // carry that huge denominator for LOSS_WINDOW_TICKS and mask real loss for the
    // next 5 ticks. Seed the counter and report no measurement on this tick.
    const bool firstVideoSample = (_lastTcpiSegsout == 0);
    const int deltaSegsout = firstVideoSample ? 0 : (tcpiSegsout - _lastTcpiSegsout);
    _lastTcpiSegsout = tcpiSegsout;

    tcpInfo tcpi;
    tcpi.rtt = rtt / 1000;
    tcpi.min_rtt = min_rtt / 1000;
    tcpi.throughput = (throughput * 8) / 1000.0;
    // loss_rate arrives as a lost-packet COUNT; the percent is Σlost/ΣsegsOut
    // over the last LOSS_WINDOW_TICKS ticks. Phase 08 (A): xbfirm divides by the
    // ONE tick's deltaSegsout, but the video flow sends only ~70-80 segments per
    // tick, so a single lost packet read as ~1.3% — over the 0.8% trigger — and
    // every retransmit was a false "high loss" that walked the bitrate down
    // (field log: 2/69=2.90%, 1/69=1.45%, 4/80=5%). The window restores the true
    // loss rate (the definition TCP itself uses) and scales with bitrate; a real
    // collapse still trips on the first tick. The C2 slot keeps the one-tick
    // form (its denominator is already large).
    if (deltaSegsout > 0) {
        m_lossWindowLost.append(static_cast<int>(loss_rate));
        m_lossWindowSegs.append(deltaSegsout);
        if (m_lossWindowLost.size() > LOSS_WINDOW_TICKS) m_lossWindowLost.removeFirst();
        if (m_lossWindowSegs.size() > LOSS_WINDOW_TICKS) m_lossWindowSegs.removeFirst();
    }
    // Phase 08 (A): freshness for THIS tick — a break needs new loss, not just an
    // elevated-but-stale window (see the member's note).
    m_videoLossThisTick = (loss_rate > 0);
    int windowLost = 0;
    int windowSegs = 0;
    for (int i = 0; i < m_lossWindowSegs.size(); ++i) {
        windowLost += m_lossWindowLost.at(i);
        windowSegs += m_lossWindowSegs.at(i);
    }
    tcpi.loss_rate = (windowSegs > 0) ? (static_cast<double>(windowLost) / windowSegs * 100.0) : 0.0;
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
        // Phase 08 (B): the link just collapsed and the video path is pinned off
        // until telemetry is clean again. Any overshoot episode in flight is
        // stale — its confirming break would otherwise land after the collapse
        // AND a recovery kick, making "2 consecutive break ticks" span a
        // discontinuity the plan never considered (H3).
        m_pendingCeiling = 0;
        m_overshootConfirmTicks = 0;
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
    if (tcpi.loss_rate <= 0.8) {
        // A clean video tick breaks any telemetry-lossy run (G3 debounce must be
        // CONSECUTIVE, not "N lossy ticks within the session").
        m_telemetryLossyTicks = 0;
        return false;
    }

    if (m_telemetryLossRate > TELEMETRY_LOSSY_THRESHOLD_PCT) {
        // C2 (the independent link probe) is ALSO lossy -> the link itself is
        // weak. xbfirm, verbatim: drop to MIN.
        //
        // PORT DEVIATION (Phase 07, G3): require TELEMETRY_LOSSY_CONFIRM_TICKS
        // consecutive lossy C2 samples first. The router's C2 flow is noisier
        // than xbfirm's probe (the reason the threshold is 0.4 not 0.3), so a
        // single noisy C2 tick must not drop the video straight to MIN — the
        // same debounce rationale as ZERO_THROUGHPUT_CONFIRM_TICKS. While
        // unconfirmed we HOLD (return true suppresses the self-congestion path
        // below, which would wrongly frame a lossy C2 tick as our own queue);
        // two consecutive lossy ticks confirm the weak link and drop to MIN.
        // L1/L2: clamp at the threshold — symmetric with the zero-throughput
        // counter; the count has done its job once confirmed and an unbounded ++
        // is pointless (the wrap is fail-safe, but no reason to allow it).
        if (m_telemetryLossyTicks < TELEMETRY_LOSSY_CONFIRM_TICKS) ++m_telemetryLossyTicks;
        if (m_telemetryLossyTicks >= TELEMETRY_LOSSY_CONFIRM_TICKS) {
            qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                                 << "% + Telemetry loss: " << m_telemetryLossRate
                                 << "% (x" << m_telemetryLossyTicks
                                 << ") -> Network degraded, dropping to MIN";
            setNewBitrate(MIN_BITRATE_KBPS);
        } else {
            qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                                 << "% + Telemetry loss: " << m_telemetryLossRate
                                 << "% (x" << m_telemetryLossyTicks << "/"
                                 << TELEMETRY_LOSSY_CONFIRM_TICKS
                                 << ") -> unconfirmed, holding one tick";
        }
    } else if (!m_videoLossThisTick) {
        // Phase 08 (A): the windowed video rate is still above 0.8% but THIS tick
        // lost nothing — the elevated window is the memory of an earlier burst,
        // not a fresh break. Return false (clean): it closes any open overshoot
        // episode (B), lets checkSenderQueue run, and stops a single burst from
        // re-firing the self-congestion path on every window tick. The
        // telemetry-lossy branch above is deliberately NOT gated by this — C2 loss
        // is fresh per tick and a weak link must still drop to MIN.
        return false;
    } else if (isOvershoot()) {
        m_telemetryLossyTicks = 0;
        // PORT DEVIATION (Phase 01): telemetry is CLEAN and the break landed
        // right after our own step-up -> this is our overshoot, not a weak
        // link. Fall back to last-good and remember it as the ceiling, so the
        // ramp holds there instead of halving-and-climbing again (the
        // sawtooth). xbfirm had no such memory; it halved unconditionally.
        // Shared with rttHigh (Phase 06) — see applyOvershootCeiling.
        const unsigned int target = applyOvershootCeiling();
        qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                             << "% + Telemetry OK (" << m_telemetryLossRate
                             << "%), still post-step-up -> overshoot;"
                             << " back to " << target << "kbps, ceiling=" << m_ceilingKbps << "kbps";
        setNewBitrate(target);
        // H1: when target == current (current <= last-good) setNewBitrate is a
        // no-op and leaves m_lastAbrAction == ABR_INCREASE, so isOvershoot() stays
        // true and the next tick re-enters this branch — re-opening the settle
        // forever under sustained loss. Record the decrease that DID happen.
        if (m_lastAbrAction == ABR_INCREASE) m_lastAbrAction = ABR_DECREASE;
    } else {
        // Telemetry clean but the break did NOT follow a recent step-up: it is
        // unrelated to our ramp.
        m_telemetryLossyTicks = 0;
        //
        // PORT DEVIATION (Phase 01, M5): xbfirm halves here (×0.50). We use
        // ABR_DECREASE_FACTOR (0.80) instead — the SAME gentle factor xbfirm
        // already applies in its rttHigh branch, so this is xbfirm's own
        // constant, not an invention.
        // Rationale: the field log shows #9 (tcpi_lost) is a 1-tick gauge, so a
        // single noisy tick tripped a full halve and produced the ~1000 kbps
        // sawtooth (1966 -> 983 -> ramp). A gentle step makes the cost of acting
        // on a noisy tick cheap, so no 2-tick debounce is needed: the reaction
        // is small enough that a false positive barely moves the bitrate, and a
        // genuine sustained loss trips on every tick, so the steps still
        // compound down to where they must. The overshoot branch (above) is what
        // caps the ceiling; this branch only sets the recovery step.
        unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * ABR_DECREASE_FACTOR);
        // PORT DEVIATION (Phase 07): the ×0.80 step is dropped by setNewBitrate
        // when it is < MIN_BITRATE_CHANGE_THRESHOLD, which happens for every
        // current below ~150 kbps — the bitrate then STALLS at ~120 with loss
        // still firing. On a sustained weak-loss break the intent IS to reach the
        // floor, so fall straight to MIN when the gentle step would be a no-op.
        bool flooredToMin = false;
        if (m_abrState.current_bitrate_kbps - new_bitrate < MIN_BITRATE_CHANGE_THRESHOLD) {
            new_bitrate = MIN_BITRATE_KBPS;
            flooredToMin = true;
        }
        qWarning().nospace() << "[ABR][" << m_iface << "] High video loss: " << tcpi.loss_rate
                             << "% but Telemetry OK (" << m_telemetryLossRate
                             << "%) -> Self-congestion, "
                             << (flooredToMin ? "gentle step below floor -> dropping to "
                                              : "reducing 20% to ")
                             << new_bitrate << "kbps";
        setNewBitrate(new_bitrate);
    }
    m_rttHistory.clear();
    return true;
}

bool XBAdaptiveBitrateStreaming::isOvershoot() const
{
    // PORT DEVIATION (Phase 01): the causal test. Both "our overshoot" and
    // "weak link" present as a low/falling video score, so a LEVEL comparison
    // cannot tell them apart. The difference is the ACTION HISTORY: an overshoot
    // break happens while we are still in "we raised and nothing has lowered
    // since" — i.e. m_lastAbrAction == ABR_INCREASE. The moment any decrease
    // fires (this branch, rttHigh, the queue guard) the action becomes
    // ABR_DECREASE and a following break is treated as a new, non-overshoot
    // event. That single transition is the whole test.
    //
    // DELIBERATELY NO time window. An earlier version required the break within
    // N ticks / M ms of the step-up, but the field log (2026-10-07) proved the
    // break lands ~26 s after the last step-up ("Cooldown check: 26016ms"), so
    // any short window missed it and any wide one would call every break an
    // overshoot. The action-history test has no clock to tune.
    //
    // Requires: (a) we are still in the post-step-up (INCREASE) state, (b) the
    // step-up recorded a healthy score AND a not-already-falling trend, and
    // (c) there IS a recorded last-good to fall back to.
    // Phase 08 (B): while an overshoot episode is OPEN (a prior break staged a
    // candidate ceiling but has not confirmed it), every following break tick is
    // still that same overshoot — even though the H1 fix already set
    // m_lastAbrAction = ABR_DECREASE. Without this early return the action test
    // below would be false on the 2nd tick and the ceiling could never confirm.
    if (m_pendingCeiling != 0) return true;

    return m_lastAbrAction == ABR_INCREASE
        && m_lastGoodKbps > 0
        && m_scoreAtStepUp >= OVERSHOOT_MIN_SCORE_AT_STEPUP
        && m_trendAtStepUp <= OVERSHOOT_MAX_TREND_AT_STEPUP;
}

bool XBAdaptiveBitrateStreaming::rttHigh(const tcpInfo &tcpi)
{
    // PORT DEVIATION (Phase 06): xbfirm reads the TELEMETRY (C2) rtt here — a
    // slot we have no source for (_haveTelemetry=false), so the delta stayed 0
    // and this branch NEVER fired. Re-pointed at the VIDEO socket's own
    // rtt-min_rtt: a delta (region-independent, not an absolute RTT) that the row
    // already carries. This branch sits in processTCPInfo's `else if`, so it only
    // runs when there is NO loss -> it is the PURE bufferbloat path (a queue
    // building before any packet is dropped), independent of the loss path.
    //
    // min_rtt==0 means the kernel has not yet sampled a minimum (fresh socket):
    // the delta would collapse to the FULL rtt, so a high-latency region would
    // look like a huge queue -> a false decrease. Guard it out. rtt<min_rtt is a
    // second inversion: the unsigned subtraction would WRAP to ~4.29e9 and fire
    // this branch falsely (and block the ramp) — require rtt>=min_rtt too.
    if (tcpi.min_rtt == 0 || tcpi.rtt < tcpi.min_rtt) return false;
    const quint32 queuing_delay = tcpi.rtt - tcpi.min_rtt;
    if (queuing_delay <= RTT_HIGH_THRESHOLD_MS) return false;

    // Phase 06: bufferbloat right after our OWN step-up IS an overshoot (we
    // pushed, the queue grew). Route it through the same ceiling logic as
    // haveLostPackets — otherwise setNewBitrate would set m_lastAbrAction =
    // ABR_DECREASE and the following loss break would look like a weak link,
    // hiding the ceiling forever.
    if (isOvershoot()) {
        const unsigned int target = applyOvershootCeiling();
        qWarning().nospace() << "[ABR][" << m_iface << "] Video queue delay > "
                             << RTT_HIGH_THRESHOLD_MS << "ms (" << queuing_delay
                             << "ms, RTT:" << tcpi.rtt << ", MinRTT:" << tcpi.min_rtt
                             << ") post-step-up -> overshoot; back to " << target
                             << "kbps, ceiling=" << m_ceilingKbps << "kbps";
        setNewBitrate(target);
        // H1: see haveLostPackets — a no-op target must still clear the
        // ABR_INCREASE state or isOvershoot() never releases.
        if (m_lastAbrAction == ABR_INCREASE) m_lastAbrAction = ABR_DECREASE;
        m_rttHistory.clear();
        return true;
    }

    qWarning().nospace() << "[ABR][" << m_iface << "] Video queue delay > "
                         << RTT_HIGH_THRESHOLD_MS << "ms (" << queuing_delay
                         << "ms, RTT:" << tcpi.rtt << ", MinRTT:" << tcpi.min_rtt
                         << "), reducing 20%";
    unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * ABR_DECREASE_FACTOR);
    // PORT DEVIATION (Phase 07, G1): at a low current the ×0.80 step falls below
    // MIN_BITRATE_CHANGE_THRESHOLD (diff<30 for current<=~145), so setNewBitrate
    // would silently no-op and this bufferbloat path would stall. Unlike the
    // weak-loss branch (which floors to MIN), a queueing-delay signal is a
    // gentler hint than packet loss — step down by exactly the threshold instead,
    // keeping the path alive without slamming to MIN. The current>=MIN guard
    // (LOW-2) keeps the subtraction from underflowing if current is ever below
    // the floor (reset() is public and does not clamp its input).
    if (m_abrState.current_bitrate_kbps - new_bitrate < MIN_BITRATE_CHANGE_THRESHOLD) {
        new_bitrate = (m_abrState.current_bitrate_kbps >= MIN_BITRATE_KBPS)
                          ? m_abrState.current_bitrate_kbps - MIN_BITRATE_CHANGE_THRESHOLD
                          : MIN_BITRATE_KBPS;
    }
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
        // PORT DEVIATION (Phase 07): require ZERO_THROUGHPUT_CONFIRM_TICKS
        // consecutive zero ticks. A single 1-tick spike (a stale delivery_rate
        // sample) must not drop the bitrate straight to MIN.
        // L2: clamp at the threshold — once confirmed the count has done its job,
        // and unbounded ++ would (theoretically) overflow int over a long outage.
        if (m_zeroThroughputTicks < ZERO_THROUGHPUT_CONFIRM_TICKS) ++m_zeroThroughputTicks;
        if (m_zeroThroughputTicks >= ZERO_THROUGHPUT_CONFIRM_TICKS) {
            qWarning().nospace() << "[ABR][" << m_iface << "] Throughput 0 for "
                                 << m_zeroThroughputTicks << " consecutive ticks! Dropping to MIN";
            setNewBitrate(MIN_BITRATE_KBPS);
        }
        return;
    }
    m_zeroThroughputTicks = 0;

    const double queuing_delay_ms = (m_avgSiocoutq / static_cast<double>(current_throughput_Bps)) * 1000.0;

    const double increase_q_delay_threshold =
        (m_lastAbrAction == ABR_DECREASE) ? QUEUE_DELAY_LOW_AFTER_DECREASE_MS : QUEUE_DELAY_LOW_THRESHOLD_MS;
    const double increase_rtt_delay_threshold =
        (m_lastAbrAction == ABR_DECREASE) ? RTT_STABLE_AFTER_DECREASE_MS : RTT_STABLE_THRESHOLD_MS;

    if (queuing_delay_ms > QUEUE_DELAY_HIGH_THRESHOLD_MS) {
        // PORT DEVIATION (Phase 06): xbfirm multiplies by m_gainDown, which the
        // (absent) CellularPredictive advisor drove. With no advisor m_gainDown
        // stays 1.0, so this decrease was a NO-OP (diff 0 < MIN_CHANGE_THRESHOLD
        // -> setNewBitrate returned early) — a dead congestion path. Use the
        // shared ABR_DECREASE_FACTOR so a drained queue actually costs 20%.
        //
        // KNOWN RESIDUAL (M1, accepted): this branch does NOT check isOvershoot(),
        // so a queue-led overshoot that drains before any loss (qdelay>400ms
        // while rtt-min<RTT_HIGH_THRESHOLD_MS, so rttHigh has not fired) sets
        // ABR_DECREASE and the ceiling is never learned for it -> a mild sawtooth
        // on this one path. Left as-is deliberately (plan Phase 06 scoped only
        // ×0.80 here); the rttHigh path DOES route through isOvershoot, so the
        // loss- and bufferbloat-led overshoots still learn a ceiling.
        qWarning().nospace() << "[ABR][" << m_iface << "] Queued time >"
                             << QUEUE_DELAY_HIGH_THRESHOLD_MS << "ms, reducing 20%";
        unsigned int decreased_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * ABR_DECREASE_FACTOR);
        // PORT DEVIATION (Phase 07, G1): same low-bitrate no-op guard as rttHigh
        // — a ×0.80 step below MIN_BITRATE_CHANGE_THRESHOLD would be dropped by
        // setNewBitrate, silently killing this congestion path. Step down by
        // exactly the threshold so the drain still happens. current>=MIN guard
        // (LOW-2) prevents underflow if current is ever below the floor.
        if (m_abrState.current_bitrate_kbps - decreased_bitrate < MIN_BITRATE_CHANGE_THRESHOLD) {
            decreased_bitrate = (m_abrState.current_bitrate_kbps >= MIN_BITRATE_KBPS)
                                    ? m_abrState.current_bitrate_kbps - MIN_BITRATE_CHANGE_THRESHOLD
                                    : MIN_BITRATE_KBPS;
        }
        setNewBitrate(decreased_bitrate);
        m_rttHistory.clear();
    } else if (queuing_delay_ms < increase_q_delay_threshold) {
        qWarning().nospace() << "[ABR][" << m_iface << "] Queued time <" << queuing_delay_ms << "ms";
        // Phase 06/07 guard: with min_rtt==0 the delta collapses to the full rtt,
        // and rtt<min_rtt wraps unsigned — either would wrongly BLOCK the ramp in
        // a high-latency region. Compute the subtraction only AFTER the guard
        // (L3) so the wrapped value is never even formed; the short-circuit made
        // it harmless before, but keeping it inside the guard is robust to a
        // future reorder.
        if (tcpi.min_rtt != 0 && tcpi.rtt >= tcpi.min_rtt) {
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
                        // Phase 08 (C): the "blocked at a LEARNED ceiling" state is
                        // `m_ceilingKbps < m_maxAbr`. At m_maxAbr there is nothing
                        // learned to hold or lift (handleRouterTick reports 0 too),
                        // so the escape clock must NOT arm there — else every
                        // healthy link at MAX would spuriously arm it and latch
                        // m_reprobeInFlight, doubling the backoff on the first real
                        // ceiling (H2).
                        if (next_bitrate > m_ceilingKbps
                            && m_ceilingKbps < m_maxAbr
                            && !allowExceedCeiling()) {
                            // Phase 08 (C): remember when the hold began, for the
                            // no-strength-source re-probe backoff. Lazy (set only if
                            // not already holding) so it measures the hold, not the
                            // last ramp tick.
                            if (m_ceilingHoldSince == 0) m_ceilingHoldSince = m_clock.elapsed();
                            qWarning().nospace() << "[ABR][" << m_iface << "] At ceiling "
                                                 << m_ceilingKbps << "kbps — holding (no ramp)";
                        } else {
                            // Phase 07: allowExceedCeiling() true (Phase 03) means the
                            // environment changed — LIFT the ceiling (re-probe). Cleared
                            // directly, no EVENT: the next SAMPLE reports 0 and the
                            // dashboard line clears; a later overshoot learns a new one.
                            //
                            // Phase 08 (C): if the lift came from the FALLBACK (no
                            // strength source) rather than a real strength recovery,
                            // flag it — a re-latch then doubles the backoff.
                            if (next_bitrate > m_ceilingKbps && m_ceilingKbps < m_maxAbr) {
                                m_reprobeInFlight = !m_networkRef.environmentChanged();
                                m_ceilingKbps = m_maxAbr;
                            }
                            m_ceilingHoldSince = 0;
                            setNewBitrate(next_bitrate);
                        }
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

unsigned int XBAdaptiveBitrateStreaming::applyOvershootCeiling()
{
    // Shared by haveLostPackets (loss break) and rttHigh (bufferbloat break).
    // M4: a break must never RAISE the bitrate. last-good is the value before our
    // step-up; if the current bitrate is already below it, falling back "up"
    // would undo a decrease and re-arm the classifier. Take the lower of the two.
    // The ceiling is floored at MIN+STEP so a recovery-from-MIN step-up can never
    // pin it to a permanent stall.
    const unsigned int target = qMin(m_lastGoodKbps, m_abrState.current_bitrate_kbps);
    const unsigned int stagedCeiling = qMax(target, MIN_BITRATE_KBPS + BITRATE_STEP_KBPS);

    // Phase 08 (B): the DECREASE is immediate and recoverable, so it happens on
    // the first break (the caller applies `target` either way). The CEILING is
    // permanent — it holds the ramp for the rest of the session — so it latches
    // only after OVERSHOOT_CONFIRM_TICKS consecutive break ticks. The first break
    // stages the candidate and opens the episode; the confirming break latches.
    // A clean tick in between (processTCPInfo(const tcpInfo&)) discards it.
    m_pendingCeiling = stagedCeiling;
    if (++m_overshootConfirmTicks < OVERSHOOT_CONFIRM_TICKS) {
        return target;   // staged only — no ceilingChanged, no settle yet
    }

    setCeiling(stagedCeiling);
    // Phase 03: capture the radio strength at THIS learn instant — the anchor
    // environmentChanged() compares against. Taken HERE, not in setCeiling():
    // setCeiling() is de-dup guarded (`if (m_ceilingKbps == kbps) return;`), so a
    // re-learn at the same plateau would skip a baseline taken there and leave a
    // stale one. applyOvershootCeiling() runs on EVERY break, so the anchor
    // always refreshes at the true learn edge.
    m_networkRef.markBaseline();
    // Phase 07: open the settle window. The loss that trails this break is the
    // queue draining; HOLD and accept it rather than decreasing below the ceiling.
    m_settleUntil = m_clock.elapsed() + OVERSHOOT_SETTLE_MS;
    // L1: processTCPInfo early-returns during settle, so haveLostPackets and
    // checkSenderQueue never run — the confirmation counters would FREEZE and a
    // pre-settle zero/lossy tick could pair with a post-settle one to look
    // "consecutive". Clear them; a real collapse re-accumulates after settle.
    m_zeroThroughputTicks = 0;
    m_telemetryLossyTicks = 0;
    // Episode complete — the next break starts a fresh one.
    m_pendingCeiling = 0;
    m_overshootConfirmTicks = 0;
    return target;
}

bool XBAdaptiveBitrateStreaming::allowExceedCeiling() const
{
    // PORT DEVIATION (Phase 03): the primary gate that may lift the learned
    // ceiling. True only when the radio has genuinely recovered since the
    // ceiling was learned — the strength baseline recorded at the learn instant
    // is LOWER than the current strength (baseline < now), held for a debounce
    // window. Absence of info (stale 603, strength==9, or no baseline yet) is
    // false, so the default is HOLD. GPS (packet 604) is deferred and becomes an
    // EXTRA gate later, not a prerequisite.
    //
    // Phase 08 (C): OR-in the FALLBACK, which only ever fires while there is NO
    // strength source at all (see ceilingReprobeDue). When a 603 exists the
    // fallback is off and this is exactly the Phase 03 behaviour.
    return m_networkRef.environmentChanged() || ceilingReprobeDue();
}

bool XBAdaptiveBitrateStreaming::ceilingReprobeDue() const
{
    // Phase 08 (C): the fallback escape. Without a USABLE strength source (no
    // fresh real level — NO_INFO and stale both count as unusable),
    // environmentChanged() is permanently false and a learned ceiling would hold
    // forever (permanent underuse). Re-probe after the current backoff of clean
    // holds; a re-overshoot re-latches and doubles the wait. Disabled the moment
    // a real level is being read — the primary path owns the escape then.
    if (m_networkRef.hasUsableStrength()) return false;
    if (m_ceilingHoldSince == 0) return false;
    return (m_clock.elapsed() - m_ceilingHoldSince) >= m_ceilingReprobeBackoffMs;
}

void XBAdaptiveBitrateStreaming::setCeiling(unsigned int kbps)
{
    // Edge-triggered: the QS channel wants ONE QS_EVT_ABR_CEILING per change, not
    // one per tick. Every write to m_ceilingKbps that represents a LEARNED limit
    // goes through here; the ctor/reset/setMaxAbrBitrate assignments (which set
    // it back to m_maxAbr = "no ceiling") do not, so they never fire a bogus
    // "ceiling found" event.
    if (m_ceilingKbps == kbps) return;
    m_ceilingKbps = kbps;
    qInfo().nospace() << "[ABR][" << m_iface << "] ceiling set to " << kbps << "kbps";
    emit ceilingChanged(m_iface, kbps);

    // Phase 08 (C): a re-latch right after a FALLBACK re-probe means that probe
    // overshot — the ceiling is real, so wait LONGER next time (backoff, capped).
    // A latch after a PRIMARY (strength-driven) lift is not a failed probe, so it
    // leaves the backoff at base.
    if (m_reprobeInFlight) {
        m_ceilingReprobeBackoffMs = qMin(m_ceilingReprobeBackoffMs * 2, CEILING_REPROBE_MAX_MS);
        m_reprobeInFlight = false;
    }
    // A fresh latch restarts the hold clock: the next re-probe is measured from here.
    m_ceilingHoldSince = 0;
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
        // last-good, and snapshot the video score/trend at the instant of the
        // step-up — the causal pair isOvershoot() reads. There is no tick/ms
        // anchor: isOvershoot() keys on the action being ABR_INCREASE, set just
        // below, which this step-up re-establishes.
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
