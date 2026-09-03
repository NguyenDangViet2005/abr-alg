#include "SRTAdaptiveBitrateStreaming.h"

SRTAdaptiveBitrateStreaming::SRTAdaptiveBitrateStreaming(QObject *parent)
    : IAdaptiveBitrateStreaming(parent)
    , m_isRunning(false)
    , m_isConnected(false)
    , m_currentBitrateKbps(DEFAULT_INITIAL_BITRATE_KBPS)
    , m_minBitrateKbps(DEFAULT_MIN_BITRATE_KBPS)
    , m_maxBitrateKbps(DEFAULT_MAX_BITRATE_KBPS)
    , m_srtLatencyMs(DEFAULT_SRT_LATENCY_MS)
    , m_rttAvg(0.0)
    , m_rttAvgDelta(0.0)
    , m_prevRtt(200.0)
    , m_rttMin(200.0)
    , m_rttJitter(0.0)
    , m_bsAvg(0.0)
    , m_bsJitter(0.0)
    , m_prevBs(0)
    , m_throughput(0.0)
    , m_nextBitrateIncr(0)
    , m_nextBitrateDecr(0)
    , m_lastLossTotal(0)
    , m_hasLastLoss(false)
    , m_lastCongestionState(CongestionState::Clear)
{
    qInfo() << "[BelaCoder-SRT] Initialized. Initial Bitrate:" << m_currentBitrateKbps
            << "kbps (Min:" << m_minBitrateKbps << ", Max:" << m_maxBitrateKbps
            << ", Rounding:" << BITRATE_ROUNDING_STEP_KBPS << "kbps, Latency:" << m_srtLatencyMs << "ms)";
}

void SRTAdaptiveBitrateStreaming::start()
{
    m_isRunning = true;
    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = 200.0;
    m_rttMin = 200.0;
    m_rttJitter = 0.0;
    m_bsAvg = 0.0;
    m_bsJitter = 0.0;
    m_prevBs = 0;
    m_throughput = 0.0;
    m_nextBitrateIncr = 0;
    m_nextBitrateDecr = 0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_lastCongestionState = CongestionState::Clear;

    qInfo() << "[BelaCoder-SRT] Started ABR service for SRT/RF link. Current Bitrate:" << m_currentBitrateKbps << "kbps";
    emit bitrateChanged(m_currentBitrateKbps);
    emit onStatus(1);
}

void SRTAdaptiveBitrateStreaming::stop()
{
    m_isRunning = false;
    qInfo() << "[BelaCoder-SRT] Stopped ABR service.";
    emit onStatus(0);
}

void SRTAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    unsigned int target = (bitrateKbps > 0) ? static_cast<unsigned int>(bitrateKbps) : DEFAULT_INITIAL_BITRATE_KBPS;
    m_currentBitrateKbps = qBound(m_minBitrateKbps, target, m_maxBitrateKbps);
    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = 200.0;
    m_rttMin = 200.0;
    m_rttJitter = 0.0;
    m_nextBitrateIncr = 0;
    m_nextBitrateDecr = 0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_lastCongestionState = CongestionState::Clear;

    qInfo() << "[BelaCoder-SRT] Reset bitrate to:" << m_currentBitrateKbps << "kbps";
    emit bitrateChanged(m_currentBitrateKbps);
}

void SRTAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{
    if (newMaxAbrBitrate >= m_minBitrateKbps) {
        m_maxBitrateKbps = newMaxAbrBitrate;
        if (m_currentBitrateKbps > m_maxBitrateKbps) {
            m_currentBitrateKbps = (m_maxBitrateKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            emit bitrateChanged(m_currentBitrateKbps);
        }
        qInfo() << "[BelaCoder-SRT] Updated Max Bitrate:" << m_maxBitrateKbps << "kbps";
    }
}

void SRTAdaptiveBitrateStreaming::setSrtLatency(int latencyMs)
{
    if (latencyMs >= 100 && latencyMs <= 10000) {
        m_srtLatencyMs = latencyMs;
        qInfo() << "[BelaCoder-SRT] Negotiated SRT Latency updated:" << m_srtLatencyMs << "ms";
    }
}

void SRTAdaptiveBitrateStreaming::handleSetMaxAbrBitrate(int maxBitrate)
{
    if (maxBitrate > 0) {
        setMaxAbrBitrate(static_cast<unsigned int>(maxBitrate));
    }
}

void SRTAdaptiveBitrateStreaming::handleSerialStatus(bool isConnected)
{
    m_isConnected = isConnected;
    qInfo() << "[BelaCoder-SRT] RF Datalink status changed:" << (isConnected ? "CONNECTED" : "DISCONNECTED");
}

void SRTAdaptiveBitrateStreaming::handleQosCameraConnection(const QVariantList &clients)
{
    if (!m_isRunning || clients.isEmpty()) {
        return;
    }

    double worstRtt = 0.0;
    double minBandwidth = 99999.0;
    double maxSendRate = 0.0;
    int maxLoss = 0;
    int maxBuffer = 0;
    bool hasValidClient = false;

    for (const QVariant &v : clients) {
        QVariantMap c = v.toMap();
        double rtt = c["msRTT"].toDouble();
        double bw = c["mbpsBandwidth"].toDouble();
        double rate = c["mbpsSendRate"].toDouble();
        int loss = c["pktSndLossTotal"].toInt();
        int buf = c.contains("pktSndBuf") ? c["pktSndBuf"].toInt() : (c.contains("byteFlightSize") ? c["byteFlightSize"].toInt() : 0);

        worstRtt = qMax(worstRtt, rtt);
        if (bw > 0.0) {
            minBandwidth = qMin(minBandwidth, bw);
        }
        maxSendRate = qMax(maxSendRate, rate);
        maxLoss = qMax(maxLoss, loss);
        maxBuffer = qMax(maxBuffer, buf);
        hasValidClient = true;
    }

    if (!hasValidClient) return;
    if (minBandwidth >= 99999.0) minBandwidth = 0.0;

    processSrtQos(worstRtt, minBandwidth, maxSendRate, maxLoss, maxBuffer);
}

void SRTAdaptiveBitrateStreaming::handleQosControllingConnection(const QVariantList &clients)
{
    Q_UNUSED(clients);
}

void SRTAdaptiveBitrateStreaming::processSrtQos(double rtt, double bandwidthMbps, double sendRateMbps, int lossTotal, int bufferSize)
{
    Q_UNUSED(sendRateMbps);
    qint64 ctime = QDateTime::currentMSecsSinceEpoch();

    // ── 1. Update Send Buffer Statistics (belacoder.c lines 190-203) ──
    int bs = bufferSize;
    if (bs >= 0) {
        m_bsAvg = m_bsAvg * 0.99 + static_cast<double>(bs) * 0.01;
        m_bsJitter *= 0.99;
        int delta_bs = bs - m_prevBs;
        if (delta_bs > m_bsJitter) {
            m_bsJitter = static_cast<double>(delta_bs);
        }
        m_prevBs = bs;
    }

    // ── 2. Update RTT Statistics (belacoder.c lines 208-238) ──
    if (m_rttAvg == 0.0) {
        m_rttAvg = rtt;
    } else {
        m_rttAvg = m_rttAvg * 0.99 + 0.01 * rtt;
    }

    double delta_rtt = rtt - m_prevRtt;
    m_rttAvgDelta = m_rttAvgDelta * 0.8 + delta_rtt * 0.2;
    m_prevRtt = rtt;

    m_rttMin *= 1.001; // slow drift upwards
    if (rtt > 0.0 && rtt != 100.0 && rtt < m_rttMin && m_rttAvgDelta < 1.0) {
        m_rttMin = rtt;
    }

    m_rttJitter *= 0.99;
    if (delta_rtt > m_rttJitter) {
        m_rttJitter = delta_rtt;
    }

    // ── 3. Update Throughput Rolling Average (belacoder.c lines 243-246) ──
    m_throughput *= 0.97;
    m_throughput += (bandwidthMbps * 1000.0) * 0.03;

    // ── 4. Packet Loss Delta Calculation ──
    int deltaLoss = 0;
    if (m_hasLastLoss) {
        deltaLoss = qMax(0, lossTotal - m_lastLossTotal);
    }
    m_lastLossTotal = lossTotal;
    m_hasLastLoss = true;

    // ── 5. Calculate Dynamic Thresholds (belacoder.c lines 256-262) ──
    int bs_th3 = static_cast<int>((m_bsAvg + m_bsJitter) * 4.0);
    int bs_th2 = static_cast<int>(qMax(50.0, m_bsAvg + qMax(m_bsJitter * 3.0, m_bsAvg)));
    int bs_th1 = static_cast<int>(qMax(50.0, m_bsAvg + m_bsJitter * 2.5));

    double rtt_th_max = m_rttAvg + qMax(m_rttJitter * 4.0, m_rttAvg * 0.15);
    double rtt_th_min = m_rttMin + qMax(1.0, m_rttJitter * 2.0);

    int bitrate = static_cast<int>(m_currentBitrateKbps);

    // ── 6. Exact 4-Zone BelaCoder Adaptation Logic (belacoder.c lines 264-282) ──

    // VÙNG 1: SẬP MẠNG KHẨN CẤP (Panic / Emergency Drop)
    if (bitrate > static_cast<int>(m_minBitrateKbps) && (rtt >= (m_srtLatencyMs / 3.0) || (bs > 0 && bs > bs_th3) || deltaLoss > 10)) {
        bitrate = m_minBitrateKbps;
        m_nextBitrateDecr = ctime + BITRATE_DECR_INT_MS;
        m_lastCongestionState = CongestionState::Panic;
    }
    // VÙNG 2: NGHẼN NẶNG (Heavy Congestion / Fast Decrease)
    else if (ctime >= m_nextBitrateDecr && (rtt > (m_srtLatencyMs / 5.0) || (bs > 0 && bs > bs_th2) || deltaLoss > 0)) {
        bitrate -= static_cast<int>(BITRATE_DECR_MIN_KBPS + (bitrate / BITRATE_DECR_SCALE));
        m_nextBitrateDecr = ctime + BITRATE_DECR_FAST_INT_MS;
        m_lastCongestionState = CongestionState::Heavy;
    }
    // VÙNG 3: NGHẼN NHẸ (Light Congestion / Step Decrease)
    else if (ctime >= m_nextBitrateDecr && (rtt > rtt_th_max || (bs > 0 && bs > bs_th1))) {
        bitrate -= static_cast<int>(BITRATE_DECR_MIN_KBPS);
        m_nextBitrateDecr = ctime + BITRATE_DECR_INT_MS;
        m_lastCongestionState = CongestionState::Light;
    }
    // VÙNG 4: MẠNG THÔNG THOÁNG & ỔN ĐỊNH (Clear / Stable Increase)
    else if (ctime >= m_nextBitrateIncr && rtt < rtt_th_min && m_rttAvgDelta < 0.01) {
        bitrate += static_cast<int>(BITRATE_INCR_MIN_KBPS + (bitrate / BITRATE_INCR_SCALE));
        m_nextBitrateIncr = ctime + BITRATE_INCR_INT_MS;
        m_lastCongestionState = CongestionState::Clear;
    }

    // ── 7. Clamp and Quantize to 50 kbps Step (Requested by user) ──
    bitrate = qBound(static_cast<int>(m_minBitrateKbps), bitrate, static_cast<int>(m_maxBitrateKbps));

    int rounded_br = (bitrate / static_cast<int>(BITRATE_ROUNDING_STEP_KBPS)) * static_cast<int>(BITRATE_ROUNDING_STEP_KBPS);
    rounded_br = qBound(static_cast<int>(m_minBitrateKbps), rounded_br, static_cast<int>(m_maxBitrateKbps));

    applyNewBitrate(static_cast<unsigned int>(rounded_br), rtt, bandwidthMbps, deltaLoss);
}

void SRTAdaptiveBitrateStreaming::applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss)
{
    if (targetBitrateKbps == m_currentBitrateKbps) {
        return;
    }

    const char *stateStr = (m_lastCongestionState == CongestionState::Panic) ? "PANIC"
                         : (m_lastCongestionState == CongestionState::Heavy) ? "HEAVY"
                         : (m_lastCongestionState == CongestionState::Light) ? "LIGHT" : "CLEAR";

    int diff = static_cast<int>(targetBitrateKbps) - static_cast<int>(m_currentBitrateKbps);

    qInfo().noquote() << QString("[BelaCoder-SRT] RTT: %1ms (min %2ms) | BW: %3 Mbps | Loss: %4 | Bitrate: %5 -> %6 kbps (%7%8) [%9]")
                .arg(rtt, 0, 'f', 1)
                .arg(m_rttMin, 0, 'f', 1)
                .arg(bandwidthMbps, 0, 'f', 2)
                .arg(deltaLoss)
                .arg(m_currentBitrateKbps)
                .arg(targetBitrateKbps)
                .arg(diff > 0 ? "+" : "")
                .arg(diff)
                .arg(stateStr);

    m_currentBitrateKbps = targetBitrateKbps;
    emit bitrateChanged(m_currentBitrateKbps);
}
