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
    , m_c2Quality(C2Quality::Good)
    , m_c2Priority(C2PriorityLevel::Normal)
    , m_isVideoEnabled(true)
    , m_isExplicitC2Only(false)
    , m_c2Rtt(0.0)
    , m_c2Retransmits(0)
    , m_c2Loss(0)
    , m_c2Unacked(0)
    , m_lastC2PacketTime(0)
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

    qInfo() << "[QoS Engine] Initialized ABR service. Default Bitrate:" << m_currentBitrateKbps << "kbps";
}

void SRTAdaptiveBitrateStreaming::start()
{
    m_isRunning = true;
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_lossHistory.clear();

    m_isVideoEnabled = true;
    m_isExplicitC2Only = false;
    m_c2Quality = C2Quality::Good;
    m_c2Priority = C2PriorityLevel::Normal;
    m_c2Rtt = 0.0;
    m_c2Retransmits = 0;
    m_c2Loss = 0;
    m_c2Unacked = 0;
    m_lastC2PacketTime = 0;

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

    qInfo() << "[QoS Engine] ABR started. Waiting for QoS data...";
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

QString SRTAdaptiveBitrateStreaming::c2QualityToString(C2Quality q) const
{
    switch (q) {
    case C2Quality::Offline:   return "OFFLINE";
    case C2Quality::Critical:  return "CRITICAL";
    case C2Quality::Poor:      return "POOR";
    case C2Quality::Fair:      return "FAIR";
    case C2Quality::Good:      return "GOOD";
    case C2Quality::Excellent: return "EXCELLENT";
    default:                   return "UNKNOWN";
    }
}

QString SRTAdaptiveBitrateStreaming::c2PriorityToString(C2PriorityLevel p) const
{
    switch (p) {
    case C2PriorityLevel::Normal:   return "NORMAL";
    case C2PriorityLevel::High:     return "HIGH";
    case C2PriorityLevel::Critical: return "CRITICAL";
    case C2PriorityLevel::C2_Only:  return "C2_ONLY";
    default:                        return "UNKNOWN";
    }
}

void SRTAdaptiveBitrateStreaming::evaluateC2Quality()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    C2Quality prevQuality = m_c2Quality;
    C2PriorityLevel prevPriority = m_c2Priority;
    bool prevVideoEnabled = m_isVideoEnabled;

    if (m_isExplicitC2Only) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false;
    }
    else if (m_lastC2PacketTime > 0 && (now - m_lastC2PacketTime) > 3500) {
        m_c2Quality = C2Quality::Offline;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false;
    }
    else if (m_c2Retransmits >= 6 || m_c2Unacked > 10 || m_c2Rtt > 250.0) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false;
    }
    else if (m_c2Retransmits >= 3 || m_c2Unacked >= 5 || m_c2Rtt > 150.0) {
        m_c2Quality = C2Quality::Poor;
        m_c2Priority = C2PriorityLevel::Critical;
        m_isVideoEnabled = true;
    }
    else if (m_c2Retransmits >= 1 || m_c2Rtt > 60.0) {
        m_c2Quality = C2Quality::Fair;
        m_c2Priority = C2PriorityLevel::High;
        m_isVideoEnabled = true;
    }
    else {
        m_c2Quality = (m_c2Rtt > 0.0 && m_c2Rtt < 30.0 && m_c2Retransmits == 0) ? C2Quality::Excellent : C2Quality::Good;
        m_c2Priority = C2PriorityLevel::Normal;
        m_isVideoEnabled = true;
    }

    if (m_isVideoEnabled != prevVideoEnabled) {
        emit videoStreamEnableChanged(m_isVideoEnabled);
        if (!m_isVideoEnabled) {
            // Camera service từ chối bitrate 0 ("Invalid bitrate: 0"),
            // nên ta hạ xuống mức sàn thấp nhất (300 kbps) để giải phóng tối đa băng thông cho C2.
            m_currentBitrateKbps = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
            emit bitrateChanged(m_currentBitrateKbps);
        } else {
            m_currentBitrateKbps = qMax(MIN_ACTIVE_VIDEO_BITRATE_KBPS, 500u);
            m_cooldownUntilMs = now + RECOVERY_COOLDOWN_MS;
            emit bitrateChanged(m_currentBitrateKbps);
        }
    }

    if (m_c2Priority != prevPriority) {
        emit c2PriorityChanged(static_cast<int>(m_c2Priority), c2PriorityToString(m_c2Priority));
    }
}

void SRTAdaptiveBitrateStreaming::handleC2ConnectionStats(const QVector<SRTPeerStat> &peers)
{
    if (peers.isEmpty()) return;
    const SRTPeerStat &p = peers.first();
    m_c2Rtt = p.msRTT;
    m_c2Retransmits = p.pktRetransTotal;
    m_c2Loss = p.pktSndLossTotal;
    m_c2Unacked = p.pktSndDropTotal;
    m_isExplicitC2Only = p.isC2Only;
    m_lastC2PacketTime = QDateTime::currentMSecsSinceEpoch();
    m_lastQosPacketTime = m_lastC2PacketTime;

    evaluateC2Quality();
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
        qInfo().noquote() << "[QoS] Waiting for QoS data...";
        return;
    }

    evaluateC2Quality();

    QString videoStr = m_isVideoEnabled ? QString("%1 kbps").arg(m_currentBitrateKbps) : "OFF";
    QString c2Str = (m_c2Priority == C2PriorityLevel::Critical || m_c2Priority == C2PriorityLevel::C2_Only) ? "CRITICAL"
                  : (m_c2Priority == C2PriorityLevel::High) ? "POOR" : "OK";

    qInfo().noquote() << QString("[QoS] RTT: %1ms | BW: %2 Mbps | Loss: %3 | Bitrate: \033[1;32m%4\033[0m | Video: %5 | C2: %6")
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestDeltaLoss)
        .arg(videoStr)
        .arg(m_isVideoEnabled ? "ON" : "OFF")
        .arg(c2Str);

    // [KEEP-ALIVE / LOCK-BITRATE]
    // Nếu bitrate đang ở trạng thái HOLD hoặc không đổi quá 3 giây,
    // định kỳ nhắc lại API Camera để ghim chặt encoder phần cứng,
    // ngăn không cho encoder camera tự động tụt bitrate khi gặp cảnh tĩnh hoặc bị reset.
    if (m_isVideoEnabled && m_currentBitrateKbps > 0 && (now - m_lastBitrateChangeTime) >= 3000) {
        m_lastBitrateChangeTime = now;
        emit bitrateChanged(m_currentBitrateKbps);
    }
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
    if (!m_isVideoEnabled) {
        return;
    }

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

    if (deltaLoss >= 8 || smoothedRtt >= (m_srtLatencyMs / 3.0) || rttInflation > 200.0) {
        state = CongestionState::Panic;
    }
    else if (deltaLoss >= 4 || rttInflation > 80.0 || smoothedRtt > (m_srtLatencyMs / 5.0)) {
        state = CongestionState::HeavyModerate;
    }
    else if (deltaLoss >= 1 || rttInflation > 35.0) {
        // Có gói rớt hoặc ping tăng nhẹ -> lập tức hạ nhẹ bitrate để giải phóng đường truyền cho video mượt
        state = CongestionState::HeavyLight;
    }
    else if (smoothedRtt > (m_rttAvg + qMax(m_rttJitter * 2.5, 10.0))) {
        state = CongestionState::Light;
    }
    else if (deltaLoss == 0) {
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
        unsigned int calculatedInitialBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
        double linkCapacityKbps = (smoothedBw > 0.0) ? (smoothedBw * 1000.0) : 2000.0;

        switch (state) {
        case CongestionState::Panic:
            calculatedInitialBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::HeavyModerate:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.40);
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::HeavyLight:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.60);
            m_cooldownUntilMs = ctime + 1500;
            break;
        case CongestionState::Light:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.70);
            break;
        case CongestionState::Clear:
        default:
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.75);
            break;
        }

        calculatedInitialBitrate = (calculatedInitialBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
        calculatedInitialBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, calculatedInitialBitrate, m_maxBitrateKbps);

        m_currentBitrateKbps = calculatedInitialBitrate;
        m_isBootstrapped = true;
        m_lastBitrateChangeTime = ctime;
        m_lastBitrateIncrTime = ctime;

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
            double safeCap = (estBwKbps > 0.0) ? (estBwKbps * 0.45) : 350.0;
            unsigned int floorBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, static_cast<unsigned int>(safeCap), 600u);
            
            if (m_currentBitrateKbps > floorBitrate) {
                unsigned int dropAmount = qMax(250u, static_cast<unsigned int>(m_currentBitrateKbps * 0.35));
                unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : floorBitrate;
                targetBitrate = qMax(floorBitrate, targetBitrate);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
    }
    else if (state == CongestionState::HeavyModerate) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;

        if (timeSinceLastChange >= BITRATE_DECR_FAST_INTERVAL_MS) {
            // Mức an toàn cho nghẽn vừa (Case 4 - 480p SD: BW 2.0M -> safeCap ~1200 - 1400 kbps)
            double safeCap = (estBwKbps > 0.0) ? (estBwKbps * 0.65) : 1000.0;
            unsigned int floorBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, static_cast<unsigned int>(safeCap * 0.85), m_maxBitrateKbps);

            if (m_currentBitrateKbps > floorBitrate) {
                unsigned int dropAmount = qMax(150u, static_cast<unsigned int>(m_currentBitrateKbps * 0.20));
                unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : floorBitrate;
                targetBitrate = qMax(floorBitrate, targetBitrate);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
    }
    else if (state == CongestionState::HeavyLight) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + 1500;

        if (timeSinceLastChange >= BITRATE_DECR_NORMAL_INTERVAL_MS) {
            // Mức an toàn cho nghẽn nhẹ (Case 3 - 720p HD: BW 3.5M -> safeCap ~2300 - 2500 kbps)
            double safeCap = (estBwKbps > 0.0) ? (estBwKbps * 0.72) : 2000.0;
            unsigned int floorBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, static_cast<unsigned int>(safeCap * 0.90), m_maxBitrateKbps);

            if (m_currentBitrateKbps > floorBitrate) {
                unsigned int dropAmount = qMax(100u, static_cast<unsigned int>(m_currentBitrateKbps * 0.12));
                unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : floorBitrate;
                targetBitrate = qMax(floorBitrate, targetBitrate);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
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
            double safeCapacity = (estBwKbps > 0.0) ? (estBwKbps * 0.70) : static_cast<double>(m_currentBitrateKbps);
            safeCapacity = qMin(safeCapacity, static_cast<double>(m_maxBitrateKbps));

            if (m_currentBitrateKbps < safeCapacity) {
                unsigned int stepKbps = 50;
                if (safeCapacity > m_currentBitrateKbps + 500.0) {
                    stepKbps = qBound(100u, static_cast<unsigned int>((safeCapacity - m_currentBitrateKbps) * 0.35), 500u);
                } else if (rttInflation < 5.0) {
                    stepKbps = 150;
                } else if (rttInflation < 15.0) {
                    stepKbps = 100;
                } else {
                    stepKbps = 50;
                }

                unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                if (targetBitrate > safeCapacity) {
                    targetBitrate = static_cast<unsigned int>(safeCapacity);
                }

                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateIncrTime = ctime;
                m_lastBitrateChangeTime = ctime;
                m_consecutiveClearCount = 0;

                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
    }
}

void SRTAdaptiveBitrateStreaming::applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss)
{
    Q_UNUSED(rtt);
    Q_UNUSED(bandwidthMbps);
    Q_UNUSED(deltaLoss);
    if (!m_isVideoEnabled && targetBitrateKbps > 0) {
        return;
    }
    if (targetBitrateKbps == m_currentBitrateKbps) {
        return;
    }

    m_currentBitrateKbps = targetBitrateKbps;
    emit bitrateChanged(m_currentBitrateKbps);
}
