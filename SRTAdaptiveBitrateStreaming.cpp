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
    , m_consecutiveZeroLossCount(0)
    , m_heartbeatTimer(nullptr)
    , m_latestSmoothedRtt(0.0)
    , m_latestSmoothedBw(0.0)
    , m_latestDeltaLoss(0)
    , m_lossPercentAvg(0.0)
    , m_latestSmoothedLossPercent(0.0)
    , m_latestStatusReason("IDLE (Cho QoS tu Server...)")
    , m_lastQosPacketTime(0)
    , m_lastLossTotal(0)
    , m_hasLastLoss(false)
    , m_lastSentTotal(0)
    , m_hasLastSent(false)
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
    m_consecutiveZeroLossCount = 0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_hasLastSent = false;
    m_lastSentTotal = 0;
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
    C2PriorityLevel prevPriority = m_c2Priority;
    bool prevVideoEnabled = m_isVideoEnabled;

    // Luồng video LUÔN ĐƯỢC DUY TRÌ BẬT (Video ON) để truyền hình ảnh camera liên tục
    m_isVideoEnabled = true;

    if (m_isExplicitC2Only) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
    }
    else {
        if (m_lastC2PacketTime > 0 && (now - m_lastC2PacketTime) > 5000) {
            m_c2Quality = C2Quality::Offline;
            m_c2Priority = C2PriorityLevel::Normal; // C2 offline do không chạy C2 thì KHÔNG được bóp nghẹt video
        }
        else if (m_c2Retransmits >= 15 || m_c2Unacked > 15 || m_c2Rtt > 300.0) {
            m_c2Quality = C2Quality::Critical;
            m_c2Priority = C2PriorityLevel::Critical;
        }
        else if (m_c2Retransmits >= 3 || m_c2Unacked >= 5 || m_c2Rtt > 150.0) {
            m_c2Quality = C2Quality::Poor;
            m_c2Priority = C2PriorityLevel::High;
        }
        else if (m_c2Retransmits >= 1 || m_c2Rtt > 60.0) {
            m_c2Quality = C2Quality::Fair;
            m_c2Priority = C2PriorityLevel::High;
        }
        else {
            m_c2Quality = (m_c2Rtt > 0.0 && m_c2Rtt < 30.0 && m_c2Retransmits == 0) ? C2Quality::Excellent : C2Quality::Good;
            m_c2Priority = C2PriorityLevel::Normal;
        }
    }

    if (m_isVideoEnabled != prevVideoEnabled) {
        emit videoStreamEnableChanged(m_isVideoEnabled);
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
    qInfo().noquote() << QString("[QoS] RTT: %1ms | BW: %2 Mbps | Loss: %3 (%4%) | Bitrate: \033[1;32m%5\033[0m | Video: %6 | State: %7")
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestDeltaLoss)
        .arg(m_latestSmoothedLossPercent, 0, 'f', 1)
        .arg(videoStr)
        .arg(m_isVideoEnabled ? "ON" : "OFF")
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
    qint64 maxSent = 0;

    for (const SRTPeerStat &peer : peers) {
        worstRtt = qMax(worstRtt, peer.msRTT);
        if (peer.mbpsBandwidth > 0.0) {
            minBandwidth = qMin(minBandwidth, peer.mbpsBandwidth);
        }
        maxSendRate = qMax(maxSendRate, peer.mbpsSendRate);
        maxLoss = qMax(maxLoss, peer.pktSndLossTotal);
        maxSent = qMax(maxSent, peer.pktSentTotal);
    }

    if (minBandwidth >= 99999.0) minBandwidth = 0.0;

    processSrtQos(worstRtt, minBandwidth, maxSendRate, maxLoss, maxSent);
}

void SRTAdaptiveBitrateStreaming::processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps, int rawLossTotal, qint64 rawSentTotal)
{
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

    // Packet Sent Delta
    qint64 deltaSent = 0;
    if (m_hasLastSent) {
        if (rawSentTotal >= m_lastSentTotal) {
            deltaSent = rawSentTotal - m_lastSentTotal;
        } else {
            deltaSent = rawSentTotal;
        }
    }
    m_lastSentTotal = rawSentTotal;
    m_hasLastSent = (rawSentTotal > 0);

    // Packet Loss Delta (xử lý cả bộ đếm tích lũy hoặc bộ đếm chu kỳ từ UDP)
    int deltaLoss = 0;
    if (m_hasLastLoss) {
        if (rawLossTotal >= m_lastLossTotal) {
            deltaLoss = rawLossTotal - m_lastLossTotal;
        } else {
            // rawLossTotal giảm -> counter reset hoặc là interval loss
            deltaLoss = rawLossTotal;
        }
    } else {
        deltaLoss = 0;
    }
    m_lastLossTotal = rawLossTotal;
    m_hasLastLoss = true;

    // Chu kỳ thời gian giữa các bản tin QoS (giây)
    double dt = (m_lastQosPacketTime > 0) ? (ctime - m_lastQosPacketTime) / 1000.0 : 0.25;
    if (dt < 0.05) dt = 0.05;
    if (dt > 2.0)  dt = 0.5;

    // Tính toán % Loss chính xác:
    // SỬA LỖI TOÁN HỌC: Mẫu số phải là tổng số gói tin ĐÃ GỬI (N_sent)!
    // Công thức cũ "deltaLoss / (expectedPkts + deltaLoss)" khiến tỷ lệ loss BỊ CHẶN TRẦN KHÔNG BAO GIỜ VƯỢT QUÁ 50%
    // (vì khi mất 80% gói thì 0.8 / (1.0 + 0.8) = 44.4% ~ 50%).
    double instantLossPercent = 0.0;
    if (deltaLoss > 0) {
        if (deltaSent > 0) {
            // 1. Dùng số gói gửi thực tế từ SRT nếu có
            instantLossPercent = (static_cast<double>(deltaLoss) / static_cast<double>(deltaSent)) * 100.0;
        } else {
            // 2. Dùng số gói gửi ước tính từ send rate / bitrate (MTU chuẩn video SRT ~1316 bytes)
            double currentRateMbps = (rawSendRateMbps > 0.1) ? rawSendRateMbps : (static_cast<double>(m_currentBitrateKbps) / 1000.0);
            double expectedPkts = (currentRateMbps * 1000000.0 * dt) / (1316.0 * 8.0);
            if (expectedPkts < 5.0) {
                expectedPkts = 5.0;
            }
            instantLossPercent = (static_cast<double>(deltaLoss) / expectedPkts) * 100.0;
        }
    }
    instantLossPercent = qBound(0.0, instantLossPercent, 100.0);

    // Làm mịn % Loss chống dao động con lắc (Anti-Limit-Cycle Oscillation):
    if (deltaLoss == 0) {
        m_consecutiveZeroLossCount++;
        // Chỉ xả loss nhanh khi mạng THỰC SỰ thông suốt liên tục (ít nhất 4 mẫu = ~1 giây không có loss)
        if (m_consecutiveZeroLossCount >= 4) {
            m_lossPercentAvg *= 0.60;
            if (m_lossPercentAvg < 1.0) {
                m_lossPercentAvg = 0.0;
            }
        } else {
            // Nếu chỉ là 1-2 mẫu rỗng ngẫu nhiên giữa chừng đợt loss, giảm nhẹ để không bị lừa chuyển trạng thái Clear
            m_lossPercentAvg *= 0.88;
        }
    } else {
        m_consecutiveZeroLossCount = 0;
        if (instantLossPercent > m_lossPercentAvg) {
            // Phản ứng nhanh khi loss đột ngột xuất hiện (chống nghẽn kịp thời)
            m_lossPercentAvg = (m_lossPercentAvg * 0.35) + (instantLossPercent * 0.65);
        } else {
            // Làm mịn khi loss giảm dần
            m_lossPercentAvg = (m_lossPercentAvg * 0.70) + (instantLossPercent * 0.30);
        }
    }
    m_latestSmoothedLossPercent = m_lossPercentAvg;

    // Khi mạng ổn định không có mất gói, cho phép m_rttMin từ từ thích ứng trượt theo baseline thực tế
    if (deltaLoss == 0 && m_lossPercentAvg < 1.0 && smoothedRtt > m_rttMin) {
        m_rttMin = (m_rttMin * 0.998) + (smoothedRtt * 0.002);
    }

    // Calculate Dynamic Metrics
    double rttInflation = (smoothedRtt > m_rttMin) ? (smoothedRtt - m_rttMin) : 0.0;

    // Phân vùng đa tầng dựa trên Tỷ lệ mất gói (% Loss) và RTT.
    // Tách biệt rõ: Không để RTT inflation đơn thuần giam lỏng trạng thái khi Loss = 0%.
    CongestionState state = CongestionState::Clear;

    if (m_lossPercentAvg >= 75.0 || (smoothedRtt >= 350.0 && rttInflation > 250.0)) {
        // Cực kỳ nghẽn - Chế độ sinh tồn (>= 75% loss, ví dụ 80% - 90% MikroTik) -> Sàn sinh tồn 400 kbps (360p Low @30fps)
        state = CongestionState::Panic;
    }
    else if (m_lossPercentAvg >= 50.0 || (smoothedRtt >= 250.0 && rttInflation > 160.0)) {
        // Rất nghẽn (50% - 75% loss, ví dụ 50% - 70% MikroTik) -> Sàn 650 kbps (360p Low @30fps)
        state = CongestionState::Extreme;
    }
    else if (m_lossPercentAvg >= 30.0 || (smoothedRtt >= 160.0 && rttInflation > 100.0)) {
        // Nghẽn nặng (30% - 50% loss) -> Sàn 1000 kbps (480p SD @30fps)
        state = CongestionState::HeavySevere;
    }
    else if (m_lossPercentAvg >= 15.0 || (smoothedRtt >= 110.0 && rttInflation > 70.0)) {
        // Nghẽn vừa (15% - 30% loss) -> Sàn 1800 kbps (720p HD @30fps)
        state = CongestionState::HeavyModerate;
    }
    else if (m_lossPercentAvg >= 5.0 || (smoothedRtt >= 80.0 && rttInflation > 50.0)) {
        // Nghẽn nhẹ (5% - 15% loss) -> Sàn 3500 kbps (1080p Full HD @30fps)
        state = CongestionState::HeavyLight;
    }
    else if (m_lossPercentAvg >= 1.5 || (smoothedRtt >= 60.0 && rttInflation > 35.0)) {
        // Dao động nhẹ / Nhiễu RF (1.5% - 5% loss) -> Sàn 4800 kbps (1080p Full HD @30fps)
        state = CongestionState::Light;
    }
    else {
        // Thông suốt (< 1.5% loss và RTT không bị phình bất thường) -> Trần tối đa 6000 kbps (1080p Full HD @30fps)
        state = CongestionState::Clear;
    }

    m_lastCongestionState = state;
    m_latestSmoothedRtt = smoothedRtt;
    m_latestSmoothedBw = smoothedBw;
    m_latestDeltaLoss = deltaLoss;
    m_lastQosPacketTime = ctime;

    // Live Bootstrapping: initial bitrate from first partition state
    if (!m_isBootstrapped) {
        unsigned int calculatedInitialBitrate = m_maxBitrateKbps;

        switch (state) {
        case CongestionState::Panic:
            calculatedInitialBitrate = 400;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::Extreme:
            calculatedInitialBitrate = 650;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::HeavySevere:
            calculatedInitialBitrate = 1000;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::HeavyModerate:
            calculatedInitialBitrate = 1800;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::HeavyLight:
            calculatedInitialBitrate = 3500;
            m_cooldownUntilMs = ctime + 1000;
            break;
        case CongestionState::Light:
            calculatedInitialBitrate = 4800;
            break;
        case CongestionState::Clear:
        default:
            calculatedInitialBitrate = m_maxBitrateKbps;
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
    if (ctime < m_cooldownUntilMs && state != CongestionState::Clear) {
        double remSec = (m_cooldownUntilMs - ctime) / 1000.0;
        m_latestStatusReason = QString("COOLDOWN (Con %1s)").arg(remSec, 0, 'f', 1);
    } else if (state == CongestionState::Clear) {
        m_latestStatusReason = "CLEAR";
    } else if (state == CongestionState::Light) {
        m_latestStatusReason = QString("LIGHT (Loss %1% - 1080p@30fps 4.8M)").arg(m_lossPercentAvg, 0, 'f', 1);
    } else if (state == CongestionState::HeavyLight) {
        m_latestStatusReason = QString("HEAVY_LIGHT (Loss %1% - 1080p@30fps 3.5M)").arg(m_lossPercentAvg, 0, 'f', 1);
    } else if (state == CongestionState::HeavyModerate) {
        m_latestStatusReason = QString("HEAVY_MODERATE (Loss %1% - 720p@30fps 1.8M)").arg(m_lossPercentAvg, 0, 'f', 1);
    } else if (state == CongestionState::HeavySevere) {
        m_latestStatusReason = QString("HEAVY_SEVERE (Loss %1% - 480p@24fps 1.0M)").arg(m_lossPercentAvg, 0, 'f', 1);
    } else if (state == CongestionState::Extreme) {
        m_latestStatusReason = QString("EXTREME (Loss %1% - 360p@20fps 650k)").arg(m_lossPercentAvg, 0, 'f', 1);
    } else {
        m_latestStatusReason = QString("PANIC_SURVIVAL (Loss %1% - 360p@20fps 400k)").arg(m_lossPercentAvg, 0, 'f', 1);
    }

    // Decision Logic: HỘI TỤ 2 CHIỀU (BIDIRECTIONAL CONVERGENCE)
    // Tự động TĂNG khi current < target, và tự động GIẢM khi current > target.
    // Loại bỏ triệt để hiện tượng treo nghẽn 900 kbps!
    qint64 timeSinceLastChange = ctime - m_lastBitrateChangeTime;

    unsigned int targetProfileBitrate = m_maxBitrateKbps;
    qint64 requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;

    switch (state) {
    case CongestionState::Panic:
        targetProfileBitrate = 400;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::Extreme:
        targetProfileBitrate = 650;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::HeavySevere:
        targetProfileBitrate = 1000;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::HeavyModerate:
        targetProfileBitrate = 1800;
        requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;
        break;
    case CongestionState::HeavyLight:
        targetProfileBitrate = 3500;
        requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;
        break;
    case CongestionState::Light:
        targetProfileBitrate = 4800;
        requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;
        break;
    case CongestionState::Clear:
    default:
        targetProfileBitrate = m_maxBitrateKbps;
        requiredInterval = BITRATE_INCR_DECISION_INTERVAL_MS;
        break;
    }

    if (state == CongestionState::Clear) {
        m_consecutiveClearCount++;

        // Kiểm tra xem bộ đệm SRT có đang bận xả gói ứ đọng (Buffer Draining Phase) hay không:
        // Nếu rttInflation >= 30ms (smoothedRtt cao hơn baseline m_rttMin), tức socket đang đẩy các gói cũ,
        // không nên vội bơm bitrate cao đè lên làm phình buffer và nghẽn xả.
        bool isBufferDrained = (rttInflation < 30.0 || smoothedRtt <= m_rttMin * 1.30 + 15.0);

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
        bool consecutiveClearMet = (m_consecutiveClearCount >= CONSECUTIVE_CLEAR_REQUIRED);

        if (cooldownExpired && decisionIntervalExpired && consecutiveClearMet) {
            if (m_currentBitrateKbps < targetProfileBitrate) {
                if (!isBufferDrained) {
                    // Đang xả buffer: giữ tốc độ bơm vừa phải để socket xả sạch hàng đợi
                    m_latestStatusReason = QString("CLEAR (Xa buffer - RTT %1ms/Base %2ms)").arg(smoothedRtt, 0, 'f', 0).arg(m_rttMin, 0, 'f', 0);
                    unsigned int stepKbps = 150;
                    unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                    targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                    targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                    m_lastBitrateIncrTime = ctime;
                    m_lastBitrateChangeTime = ctime;
                    applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
                } else {
                    // Buffer đã xả sạch: TĂNG TỐC PHỤC HỒI NHANH (Fast Recovery Ramp-up)
                    // Nâng nhanh bitrate lên 6000 kbps trong 3-4 giây để camera đạt Full HD ngay
                    unsigned int stepKbps = 800;
                    if (m_currentBitrateKbps < 2000) {
                        stepKbps = 800;
                    } else if (m_currentBitrateKbps < 4000) {
                        stepKbps = 1000;
                    } else {
                        stepKbps = 1000;
                    }

                    unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
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
    else {
        // Các trạng thái nghẽn: HỘI TỤ 2 CHIỀU (BIDIRECTIONAL ADAPTATION)
        // 1. Nếu bitrate hiện tại > target: Giảm dứt khoát NGAY LẬP TỨC về targetProfileBitrate (Fast Emergency Drop)
        // Khi xảy ra mất gói (loss >= 15%), nếu giảm từ từ từng bước thì trong 2-3 giây trễ đó camera vẫn bơm bitrate cao,
        // làm nghẽn thêm và tràn hàng đợi SRT/GStreamer -> gây phình buffer (bloat), giật hình và sập luồng.
        if (m_currentBitrateKbps > targetProfileBitrate) {
            m_consecutiveClearCount = 0;
            if (timeSinceLastChange >= requiredInterval) {
                unsigned int targetBitrate = targetProfileBitrate;
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
        // 2. Nếu bitrate hiện tại < target: TỰ ĐỘNG TĂNG LÊN ĐẾN TARGET NẤC ĐÓ!
        // (Ví dụ vừa từ Panic 400k lên HeavyModerate 1800k do loss giảm từ 90% xuống 30%)
        else if (m_currentBitrateKbps < targetProfileBitrate) {
            m_consecutiveClearCount = 0;
            bool cooldownExpired = (ctime >= m_cooldownUntilMs);
            bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);

            if (cooldownExpired && decisionIntervalExpired) {
                unsigned int stepUp = 400;
                unsigned int targetBitrate = m_currentBitrateKbps + stepUp;
                targetBitrate = qMin(targetProfileBitrate, targetBitrate);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateIncrTime = ctime;
                m_lastBitrateChangeTime = ctime;
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

    // Chỉ bóp bitrate video khi C2 ở trạng thái tường minh C2_Only từ telemetry
    if (m_isExplicitC2Only && targetBitrateKbps > 400) {
        targetBitrateKbps = 400;
    }

    targetBitrateKbps = (targetBitrateKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
    targetBitrateKbps = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrateKbps, m_maxBitrateKbps);

    if (targetBitrateKbps == m_currentBitrateKbps) {
        return;
    }

    m_currentBitrateKbps = targetBitrateKbps;
    emit bitrateChanged(m_currentBitrateKbps);
}
