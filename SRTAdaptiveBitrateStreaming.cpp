#include "SRTAdaptiveBitrateStreaming.h"
#include <algorithm>

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
    , m_prevRtt(25.0)
    , m_rttMin(25.0)
    , m_rttJitter(0.0)
    , m_throughput(0.0)
    , m_lastBitrateChangeTime(0)
    , m_lastBitrateIncrTime(0)
    , m_cooldownUntilMs(0)
    , m_consecutiveClearCount(0)
    , m_heartbeatTimer(nullptr)
    , m_latestSmoothedRtt(0.0)
    , m_latestSmoothedBw(0.0)
    , m_latestDeltaLoss(0)
    , m_latestStatusReason("IDLE (Cho QoS tu Server...)")
    , m_lastQosPacketTime(0)
    , m_lastLossTotal(0)
    , m_hasLastLoss(false)
    , m_isBootstrapped(false)
    , m_lastCongestionState(CongestionState::Clear)
{
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &SRTAdaptiveBitrateStreaming::onHeartbeatTimeout);

    qInfo() << "[BelaCoder-SRT] Initialized Anti-Oscillation ABR. Initial Bitrate:" << m_currentBitrateKbps
            << "kbps (Min:" << m_minBitrateKbps << ", Max:" << m_maxBitrateKbps
            << ", Rounding:" << BITRATE_ROUNDING_STEP_KBPS << "kbps, Clear Required:" << CONSECUTIVE_CLEAR_REQUIRED << ")";
}

void SRTAdaptiveBitrateStreaming::start()
{
    m_isRunning = true;
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_lossHistory.clear();

    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = 25.0;
    m_rttMin = 25.0;
    m_rttJitter = 0.0;
    m_throughput = 0.0;

    m_lastBitrateChangeTime = QDateTime::currentMSecsSinceEpoch();
    m_lastBitrateIncrTime = m_lastBitrateChangeTime;
    m_cooldownUntilMs = 0;
    m_consecutiveClearCount = 0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_isBootstrapped = false;
    m_lastCongestionState = CongestionState::Clear;
    m_latestStatusReason = "IDLE (Cho QoS tu Server...)";
    m_lastQosPacketTime = 0;

    if (m_heartbeatTimer) {
        m_heartbeatTimer->start(1000);
    }

    qInfo() << "[BelaCoder-SRT] Started ABR service. Waiting for live network stats...";
    emit onStatus(1);
}

void SRTAdaptiveBitrateStreaming::stop()
{
    m_isRunning = false;
    if (m_heartbeatTimer && m_heartbeatTimer->isActive()) {
        m_heartbeatTimer->stop();
    }
    qInfo() << "[BelaCoder-SRT] Stopped ABR service.";
    emit onStatus(0);
}

void SRTAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    unsigned int target = (bitrateKbps > 0) ? static_cast<unsigned int>(bitrateKbps) : DEFAULT_INITIAL_BITRATE_KBPS;
    m_currentBitrateKbps = qBound(m_minBitrateKbps, target, m_maxBitrateKbps);
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_lossHistory.clear();

    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = 25.0;
    m_rttMin = 25.0;
    m_rttJitter = 0.0;
    m_lastBitrateChangeTime = QDateTime::currentMSecsSinceEpoch();
    m_lastBitrateIncrTime = m_lastBitrateChangeTime;
    m_cooldownUntilMs = 0;
    m_consecutiveClearCount = 0;
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
        qInfo() << "[BelaCoder-SRT] SRT Latency updated:" << m_srtLatencyMs << "ms";
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
    qInfo() << "[BelaCoder-SRT] RF Datalink status:" << (isConnected ? "CONNECTED" : "DISCONNECTED");
}

double SRTAdaptiveBitrateStreaming::calculateMedian(QVector<double> list)
{
    if (list.isEmpty()) return 0.0;
    std::sort(list.begin(), list.end());
    int mid = list.size() / 2;
    if (list.size() % 2 != 0) {
        return list.at(mid);
    }
    return (list.at(mid - 1) + list.at(mid)) / 2.0;
}

double SRTAdaptiveBitrateStreaming::calculateAverage(const QVector<double> &list)
{
    if (list.isEmpty()) return 0.0;
    double sum = 0.0;
    for (double v : list) sum += v;
    return sum / static_cast<double>(list.size());
}

void SRTAdaptiveBitrateStreaming::handleC2ConnectionStats(const QVector<SRTPeerStat> &peers)
{
    Q_UNUSED(peers);
    m_lastQosPacketTime = QDateTime::currentMSecsSinceEpoch();
}

void SRTAdaptiveBitrateStreaming::handleQosControllingConnection(const QVector<SRTPeerStat> &peers)
{
    if (peers.isEmpty()) return;
    handleC2ConnectionStats(peers);
}

void SRTAdaptiveBitrateStreaming::onHeartbeatTimeout()
{
    if (!m_isRunning) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool hasRecentQos = (m_lastQosPacketTime > 0 && (now - m_lastQosPacketTime) < 3000);

    if (!hasRecentQos) {
        qInfo().noquote() << QString("[BelaCoder-SRT Status] RTT: --ms | BW: 0.00 Mbps | Loss: 0 | Bitrate: %1 kbps | IDLE (Chua co QoS tu Server...)")
                    .arg(m_currentBitrateKbps);
        return;
    }

    qInfo().noquote() << QString("[BelaCoder-SRT Status] SRT RTT: %1ms (min %2ms) | BW: %3 Mbps | Loss: %4 | Bitrate: %5 kbps | %6")
                .arg(m_latestSmoothedRtt, 0, 'f', 1)
                .arg(m_rttMin, 0, 'f', 1)
                .arg(m_latestSmoothedBw, 0, 'f', 2)
                .arg(m_latestDeltaLoss)
                .arg(m_currentBitrateKbps)
                .arg(m_latestStatusReason);
}

void SRTAdaptiveBitrateStreaming::handleQosCameraConnection(const QVector<SRTPeerStat> &peers)
{
    if (!m_isRunning || peers.isEmpty()) {
        return;
    }

    double worstRtt = 0.0;
    double minBandwidth = 99999.0;
    double maxSendRate = 0.0;
    int maxLoss = 0;

    for (const SRTPeerStat &peer : peers) {
        worstRtt = qMax(worstRtt, peer.msRTT);
        if (peer.mbpsBandwidth > 0.0) {
            minBandwidth = qMin(minBandwidth, peer.mbpsBandwidth);
        }
        maxSendRate = qMax(maxSendRate, peer.mbpsSendRate);
        maxLoss = qMax(maxLoss, peer.pktSndLossTotal);
    }

    if (minBandwidth >= 99999.0) minBandwidth = 0.0;

    processSrtQos(worstRtt, minBandwidth, maxSendRate, maxLoss);
}

void SRTAdaptiveBitrateStreaming::processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps, int rawLossTotal)
{
    Q_UNUSED(rawSendRateMbps);
    qint64 ctime = QDateTime::currentMSecsSinceEpoch();

    // Sliding Window Smoothing
    if (rawRtt > 0.0) {
        m_rttHistory.append(rawRtt);
        if (m_rttHistory.size() > SLIDING_WINDOW_SIZE) m_rttHistory.removeFirst();
    }
    if (rawBandwidthMbps > 0.0) {
        m_bwHistory.append(rawBandwidthMbps);
        if (m_bwHistory.size() > SLIDING_WINDOW_SIZE) m_bwHistory.removeFirst();
    }

    double smoothedRtt = (m_rttHistory.isEmpty()) ? rawRtt : calculateMedian(m_rttHistory);
    double smoothedBw = (m_bwHistory.isEmpty()) ? rawBandwidthMbps : calculateAverage(m_bwHistory);

    if (smoothedRtt < MIN_VALID_RTT_MS) {
        smoothedRtt = MIN_VALID_RTT_MS;
    }

    // Update RTT Statistics & Thresholds
    if (m_rttAvg == 0.0) {
        m_rttAvg = smoothedRtt;
    } else {
        m_rttAvg = m_rttAvg * 0.95 + 0.05 * smoothedRtt;
    }

    double delta_rtt = smoothedRtt - m_prevRtt;
    m_rttAvgDelta = m_rttAvgDelta * 0.8 + delta_rtt * 0.2;
    m_prevRtt = smoothedRtt;

    m_rttMin *= 1.0005;
    if (smoothedRtt >= MIN_VALID_RTT_MS && smoothedRtt < m_rttMin && m_rttAvgDelta < 0.5) {
        m_rttMin = smoothedRtt;
    }
    if (m_rttMin < MIN_VALID_RTT_MS) {
        m_rttMin = MIN_VALID_RTT_MS;
    }

    m_rttJitter *= 0.95;
    if (delta_rtt > m_rttJitter) {
        m_rttJitter = delta_rtt;
    }

    // Packet Loss Delta
    int deltaLoss = 0;
    if (m_hasLastLoss) {
        deltaLoss = qMax(0, rawLossTotal - m_lastLossTotal);
    }
    m_lastLossTotal = rawLossTotal;
    m_hasLastLoss = true;

    // Calculate Dynamic Metrics
    double rttInflation = (smoothedRtt > m_rttMin) ? (smoothedRtt - m_rttMin) : 0.0;
    double estBwKbps = smoothedBw * 1000.0;

    // Congestion Classification
    CongestionState state = CongestionState::Clear;

    if (smoothedRtt >= (m_srtLatencyMs / 3.0) || rttInflation > 250.0 || (deltaLoss >= 12 && rttInflation > 40.0)) {
        state = CongestionState::Panic;
    }
    else if (rttInflation > 120.0 || (deltaLoss >= 4 && rttInflation > 35.0) || smoothedRtt > (m_srtLatencyMs / 5.0)) {
        state = CongestionState::HeavyModerate;
    }
    else if (rttInflation > 60.0 || (deltaLoss >= 2 && rttInflation > 20.0) || deltaLoss >= 8) {
        state = CongestionState::HeavyLight;
    }
    else if (deltaLoss > 0 || smoothedRtt > (m_rttAvg + qMax(m_rttJitter * 3.0, m_rttAvg * 0.15))) {
        state = CongestionState::Light;
    }
    else if (deltaLoss == 0 && m_rttAvgDelta < 2.0) {
        state = CongestionState::Clear;
    }
    else {
        state = CongestionState::Light;
    }

    m_lastCongestionState = state;
    m_latestSmoothedRtt = smoothedRtt;
    m_latestSmoothedBw = smoothedBw;
    m_latestDeltaLoss = deltaLoss;
    m_lastQosPacketTime = ctime;

    // Live Bootstrapping: initial bitrate from first partition state
    if (!m_isBootstrapped) {
        unsigned int calculatedInitialBitrate = m_minBitrateKbps;
        double linkCapacityKbps = (smoothedBw > 0.0) ? (smoothedBw * 1000.0) : 2000.0;
        QString stateName;

        switch (state) {
        case CongestionState::Panic:
            calculatedInitialBitrate = m_minBitrateKbps;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            stateName = "PANIC";
            break;
        case CongestionState::HeavyModerate:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.40);
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            stateName = "HEAVY_MODERATE";
            break;
        case CongestionState::HeavyLight:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.60);
            m_cooldownUntilMs = ctime + 1500;
            stateName = "HEAVY_LIGHT";
            break;
        case CongestionState::Light:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.70);
            stateName = "LIGHT/HOLD";
            break;
        case CongestionState::Clear:
        default:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.80);
            stateName = "CLEAR";
            break;
        }

        calculatedInitialBitrate = (calculatedInitialBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
        calculatedInitialBitrate = qBound(m_minBitrateKbps, calculatedInitialBitrate, m_maxBitrateKbps);

        m_currentBitrateKbps = calculatedInitialBitrate;
        m_isBootstrapped = true;
        m_lastBitrateChangeTime = ctime;
        m_lastBitrateIncrTime = ctime;

        qInfo().noquote() << QString("[BelaCoder-SRT] LIVE BOOTSTRAP: Initial Bitrate via [%1]: %2 kbps (BW: %3 Mbps, RTT: %4ms, Loss: %5)")
                    .arg(stateName)
                    .arg(m_currentBitrateKbps)
                    .arg(smoothedBw, 0, 'f', 2)
                    .arg(smoothedRtt, 0, 'f', 1)
                    .arg(rawLossTotal);

        emit bitrateChanged(m_currentBitrateKbps);
        return;
    }

    // Status reason
    if (ctime < m_cooldownUntilMs) {
        double remSec = (m_cooldownUntilMs - ctime) / 1000.0;
        m_latestStatusReason = QString("COOLDOWN (Con %1s)").arg(remSec, 0, 'f', 1);
    } else if (state == CongestionState::Clear) {
        m_latestStatusReason = QString("CLEAR (Dem %1/%2 sample de tang)").arg(m_consecutiveClearCount).arg(CONSECUTIVE_CLEAR_REQUIRED);
    } else if (state == CongestionState::Light) {
        if (deltaLoss > 0 && rttInflation <= 20.0) {
            m_latestStatusReason = QString("HOLD (Nhieu RF - Rot %1 goi, RTT tot -> Giu nguyen bitrate)").arg(deltaLoss);
        } else {
            m_latestStatusReason = "HOLD (Mang on dinh / Giu nguyen bitrate)";
        }
    } else if (state == CongestionState::HeavyLight) {
        m_latestStatusReason = "HEAVY_LIGHT (Giam nhe 10-15%)";
    } else if (state == CongestionState::HeavyModerate) {
        m_latestStatusReason = "HEAVY_MODERATE (Giam vua 20-25%)";
    } else {
        m_latestStatusReason = "PANIC (Sap mang / Giam manh 40%)";
    }

    // Decision Logic & Anti-Oscillation
    qint64 timeSinceLastChange = ctime - m_lastBitrateChangeTime;

    if (state == CongestionState::Panic) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;

        if (timeSinceLastChange >= BITRATE_DECR_FAST_INTERVAL_MS) {
            unsigned int dropAmount = qMax(400u, static_cast<unsigned int>(m_currentBitrateKbps * 0.40));
            unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : m_minBitrateKbps;

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateChangeTime = ctime;
            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
    else if (state == CongestionState::HeavyModerate) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;

        if (timeSinceLastChange >= BITRATE_DECR_FAST_INTERVAL_MS) {
            unsigned int dropAmount = qMax(200u, static_cast<unsigned int>(m_currentBitrateKbps * 0.22));
            unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : m_minBitrateKbps;

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateChangeTime = ctime;
            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
    else if (state == CongestionState::HeavyLight) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + 1500;

        if (timeSinceLastChange >= BITRATE_DECR_NORMAL_INTERVAL_MS) {
            unsigned int dropAmount = qMax(100u, static_cast<unsigned int>(m_currentBitrateKbps * 0.12));
            unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : m_minBitrateKbps;

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateChangeTime = ctime;
            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
    else if (state == CongestionState::Light) {
        m_consecutiveClearCount = 0;
    }
    else if (state == CongestionState::Clear) {
        m_consecutiveClearCount++;

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
        bool consecutiveClearMet = (m_consecutiveClearCount >= CONSECUTIVE_CLEAR_REQUIRED);

        if (cooldownExpired && decisionIntervalExpired && consecutiveClearMet) {
            unsigned int stepKbps = 50;
            if (rttInflation < 5.0) {
                stepKbps = 150;
            } else if (rttInflation < 15.0) {
                stepKbps = 100;
            } else {
                stepKbps = 50;
            }

            unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;

            if (estBwKbps > 0.0 && rttInflation > 60.0 && targetBitrate > estBwKbps * 0.90) {
                targetBitrate = static_cast<unsigned int>(estBwKbps * 0.90);
            }

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateIncrTime = ctime;
            m_lastBitrateChangeTime = ctime;
            m_consecutiveClearCount = 0;

            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
}

void SRTAdaptiveBitrateStreaming::applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss)
{
    if (targetBitrateKbps == m_currentBitrateKbps) {
        return;
    }

    const char *stateStr = (m_lastCongestionState == CongestionState::Panic) ? "PANIC"
                         : (m_lastCongestionState == CongestionState::HeavySevere) ? "HEAVY_SEVERE"
                         : (m_lastCongestionState == CongestionState::HeavyModerate) ? "HEAVY_MODERATE"
                         : (m_lastCongestionState == CongestionState::HeavyLight) ? "HEAVY_LIGHT"
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
