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
    , m_c2RttVar(0.0)
    , m_c2Retransmits(0)
    , m_c2Unacked(0)
    , m_c2Loss(0)
    , m_lastC2PacketTime(0)
    , m_rttAvg(0.0)
    , m_rttAvgDelta(0.0)
    , m_prevRtt(25.0)
    , m_rttMin(25.0)
    , m_rttJitter(0.0)
    , m_bsAvg(0.0)
    , m_bsJitter(0.0)
    , m_prevBs(0)
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

    qInfo() << "[BelaCoder-SRT] Initialized Anti-Oscillation ABR with C2 Priority. Initial Bitrate:" << m_currentBitrateKbps
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
    m_bsAvg = 0.0;
    m_bsJitter = 0.0;
    m_prevBs = 0;
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
        m_heartbeatTimer->start(1000); // Bắt đầu in log Heartbeat mỗi giây ngay khi app khởi động
    }

    qInfo() << "[BelaCoder-SRT] Started ABR service for SRT/RF link. Waiting for live network stats to compute initial bitrate...";
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

    // 0. Nhận diện rõ ràng lệnh C2_ONLY từ telemetry packet
    if (m_isExplicitC2Only) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false; // TẮT VIDEO HOÀN TOÀN để bảo vệ an toàn bay
    }
    // 1. Kiểm tra Liveness (nếu quá 3.5s không có C2 telemetry -> Mất sóng C2)
    else if (m_lastC2PacketTime > 0 && (now - m_lastC2PacketTime) > 3500) {
        m_c2Quality = C2Quality::Offline;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false; // TẮT VIDEO HOÀN TOÀN để bảo vệ an toàn bay
    }
    // 2. Vùng Nguy hiểm (Critical: Unacked lớn > 10, Retransmit >= 3, hoặc RTT > 250ms)
    else if (m_c2Unacked > 10 || m_c2Retransmits >= 3 || m_c2Rtt > 250.0) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
        m_isVideoEnabled = false; // TẮT VIDEO để nhường 100% tài nguyên cho C2
    }
    // 3. Vùng Xấu (Poor: Unacked >= 3, Retransmit >= 1, hoặc RTT > 120ms)
    else if (m_c2Unacked >= 3 || m_c2Retransmits >= 1 || m_c2Rtt > 120.0) {
        m_c2Quality = C2Quality::Poor;
        m_c2Priority = C2PriorityLevel::Critical;
        m_isVideoEnabled = true; // Video vẫn bật nhưng bị bóp nghẹt bitrate
    }
    // 4. Vùng Chớm chập chờn (Fair: RTT > 60ms hoặc RTT Variance > 30ms)
    else if (m_c2Rtt > 60.0 || m_c2RttVar > 30.0) {
        m_c2Quality = C2Quality::Fair;
        m_c2Priority = C2PriorityLevel::High;
        m_isVideoEnabled = true;
    }
    // 5. Vùng Tốt / Xuất sắc (Good / Excellent)
    else {
        m_c2Quality = (m_c2Rtt > 0.0 && m_c2Rtt < 25.0 && m_c2Retransmits == 0) ? C2Quality::Excellent : C2Quality::Good;
        m_c2Priority = C2PriorityLevel::Normal;
        m_isVideoEnabled = true;
    }

    // Phát tín hiệu khi có sự thay đổi về Video State hoặc C2 Priority
    if (m_isVideoEnabled != prevVideoEnabled) {
        qWarning().noquote() << QString(">>> [C2 ARBITRATION] Video Stream State Changed: %1 (C2 Quality: %2, C2 Priority: %3) <<<")
                    .arg(m_isVideoEnabled ? "ENABLED (ON)" : "DISABLED (OFF - C2 ONLY)")
                    .arg(c2QualityToString(m_c2Quality))
                    .arg(c2PriorityToString(m_c2Priority));
        emit videoStreamEnableChanged(m_isVideoEnabled);

        if (!m_isVideoEnabled) {
            m_currentBitrateKbps = 0;
            emit bitrateChanged(0);
        }
    }

    if (m_c2Priority != prevPriority) {
        qInfo().noquote() << QString("[C2 Priority Engine] Priority Level Changed: %1 -> %2 (C2 RTT: %3ms, Unacked: %4, Retrans: %5)")
                    .arg(c2PriorityToString(prevPriority))
                    .arg(c2PriorityToString(m_c2Priority))
                    .arg(m_c2Rtt, 0, 'f', 1)
                    .arg(m_c2Unacked)
                    .arg(m_c2Retransmits);
        emit c2PriorityChanged(static_cast<int>(m_c2Priority), c2PriorityToString(m_c2Priority));
    }
}

void SRTAdaptiveBitrateStreaming::handleC2ConnectionStats(const QVariantMap &c2Stats)
{
    m_c2Rtt = c2Stats.value("rtt_ms", 0.0).toDouble();
    m_c2RttVar = c2Stats.value("rtt_var_ms", 0.0).toDouble();
    m_c2Retransmits = c2Stats.value("retransmits", 0).toInt();
    m_c2Loss = c2Stats.value("tcpi_loss", 0).toInt();
    m_c2Unacked = c2Stats.value("unacked_pkts", 0).toInt();
    m_lastC2PacketTime = QDateTime::currentMSecsSinceEpoch();

    bool c2OnlyFlag = c2Stats.value("c2_only", false).toBool();
    QString congState = c2Stats.value("congestion_state", "").toString().toUpper();
    QString prioState = c2Stats.value("priority", "").toString().toUpper();
    m_isExplicitC2Only = (c2OnlyFlag || congState == "C2_ONLY" || prioState == "C2_ONLY");

    evaluateC2Quality();
}

void SRTAdaptiveBitrateStreaming::handleQosControllingConnection(const QVariantList &clients)
{
    if (clients.isEmpty()) return;
    QVariantMap c = clients.first().toMap();
    handleC2ConnectionStats(c);
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

    evaluateC2Quality();

    QString c2Info = QString("C2: %1 (RTT: %2ms, Prio: %3)")
                         .arg(c2QualityToString(m_c2Quality))
                         .arg(m_c2Rtt, 0, 'f', 1)
                         .arg(c2PriorityToString(m_c2Priority));

    QString videoStateStr = m_isVideoEnabled ? QString("%1 kbps").arg(m_currentBitrateKbps) : "OFF (C2_ONLY)";

    qInfo().noquote() << QString("[BelaCoder-SRT Status] SRT RTT: %1ms (min %2ms) | BW: %3 Mbps | Loss: %4 | Video: %5 | %6 | %7")
                .arg(m_latestSmoothedRtt, 0, 'f', 1)
                .arg(m_rttMin, 0, 'f', 1)
                .arg(m_latestSmoothedBw, 0, 'f', 2)
                .arg(m_latestDeltaLoss)
                .arg(videoStateStr)
                .arg(c2Info)
                .arg(m_latestStatusReason);
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

void SRTAdaptiveBitrateStreaming::processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps, int rawLossTotal, int rawBufferSize)
{
    Q_UNUSED(rawSendRateMbps);
    qint64 ctime = QDateTime::currentMSecsSinceEpoch();

    // ── 1. Sliding Window Smoothing (🟡 Mục 7) ──
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

    // Sanity check RTT (🟡 Mục 8: loại bỏ giá trị bất thường < 5ms trên Wi-Fi/RF)
    if (smoothedRtt < MIN_VALID_RTT_MS) {
        smoothedRtt = MIN_VALID_RTT_MS;
    }

    // ── 2. Update Send Buffer Statistics ──
    int bs = rawBufferSize;
    if (bs >= 0) {
        m_bsAvg = m_bsAvg * 0.95 + static_cast<double>(bs) * 0.05;
        m_bsJitter *= 0.95;
        int delta_bs = bs - m_prevBs;
        if (delta_bs > m_bsJitter) {
            m_bsJitter = static_cast<double>(delta_bs);
        }
        m_prevBs = bs;
    }

    // ── 4. Update RTT Statistics & Thresholds ──
    if (m_rttAvg == 0.0) {
        m_rttAvg = smoothedRtt;
    } else {
        m_rttAvg = m_rttAvg * 0.95 + 0.05 * smoothedRtt;
    }

    double delta_rtt = smoothedRtt - m_prevRtt;
    m_rttAvgDelta = m_rttAvgDelta * 0.8 + delta_rtt * 0.2;
    m_prevRtt = smoothedRtt;

    // Slow upwards drift for min RTT floor
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

    // ── 5. Packet Loss Delta ──
    int deltaLoss = 0;
    if (m_hasLastLoss) {
        deltaLoss = qMax(0, rawLossTotal - m_lastLossTotal);
    }
    m_lastLossTotal = rawLossTotal;
    m_hasLastLoss = true;

    // ── 6. Calculate Dynamic Metrics ──
    double rttInflation = (smoothedRtt > m_rttMin) ? (smoothedRtt - m_rttMin) : 0.0;
    double rtt_th_max = m_rttAvg + qMax(m_rttJitter * 3.0, m_rttAvg * 0.15);
    double rtt_th_min = m_rttMin + qMax(2.0, m_rttJitter * 1.5);
    double estBwKbps = smoothedBw * 1000.0;

    int bs_th3 = static_cast<int>((m_bsAvg + m_bsJitter) * 4.0);
    int bs_th2 = static_cast<int>(qMax(50.0, m_bsAvg + qMax(m_bsJitter * 3.0, m_bsAvg)));
    int bs_th1 = static_cast<int>(qMax(50.0, m_bsAvg + m_bsJitter * 2.5));

    // ── 7. Phân cấp mức độ nghẽn (Phân biệt Nhiễu RF ngẫu nhiên vs Nghẽn mạng thật) ──
    CongestionState state = CongestionState::Clear;

    // VÙNG SẬP / KHẨN CẤP (Panic): Mạng nghẽn nghiêm trọng hoặc sập sóng
    if (smoothedRtt >= (m_srtLatencyMs / 3.0) || rttInflation > 250.0 || (deltaLoss >= 12 && rttInflation > 40.0) || (bs > 0 && bs > bs_th3)) {
        state = CongestionState::Panic;
    }
    // VÙNG NGHẼN NẶNG (Heavy Moderate - 20-25%): Nghẽn thật sự (Loss đi kèm trễ dồn RTT > 35ms)
    else if (rttInflation > 120.0 || (deltaLoss >= 4 && rttInflation > 35.0) || (bs > 0 && bs > bs_th2) || smoothedRtt > (m_srtLatencyMs / 5.0)) {
        state = CongestionState::HeavyModerate;
    }
    // VÙNG NGHẼN NHẸ (Heavy Light - 10-15%): Chớm nghẽn (RTT tăng nhẹ > 60ms) hoặc Loss dồn dập (>= 8 gói)
    else if (rttInflation > 60.0 || (deltaLoss >= 2 && rttInflation > 20.0) || deltaLoss >= 8) {
        state = CongestionState::HeavyLight;
    }
    // VÙNG NHIỄU SÓNG RF HOẶC GIỮ NHỊP (Hold / Light Congestion)
    // 1. Nhiễu RF ngẫu nhiên: rớt 1-4 gói nhưng RTT vẫn rất thấp (rttInflation <= 20ms) -> SRT ARQ tự sửa, KHÔNG hạ bitrate!
    // 2. RTT dao động nhẹ trên mức min
    else if (deltaLoss > 0 || smoothedRtt > rtt_th_max || (bs > 0 && bs > bs_th1)) {
        state = CongestionState::Light;
    }
    // VÙNG THÔNG THOÁNG (Clear): Sạch loss (deltaLoss == 0) và RTT ổn định
    else if (deltaLoss == 0 && m_rttAvgDelta < 2.0) {
        state = CongestionState::Clear;
    }
    else {
        state = CongestionState::Light; // Default safe hold
    }

    m_lastCongestionState = state;
    m_latestSmoothedRtt = smoothedRtt;
    m_latestSmoothedBw = smoothedBw;
    m_latestDeltaLoss = deltaLoss;
    m_lastQosPacketTime = ctime;

    // ── 7.5. Live Bootstrapping: Định vị Bitrate ban đầu theo đúng phân vùng mạng chuẩn ──
    if (!m_isBootstrapped) {
        unsigned int calculatedInitialBitrate = m_minBitrateKbps;
        double linkCapacityKbps = (smoothedBw > 0.0) ? (smoothedBw * 1000.0) : static_cast<double>(DEFAULT_INITIAL_BITRATE_KBPS);
        QString stateName;

        switch (state) {
        case CongestionState::Panic:
            // Sập sóng ngay từ đầu -> Khởi tạo ở mức sàn an toàn nhất
            calculatedInitialBitrate = m_minBitrateKbps;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            stateName = "PANIC";
            break;

        case CongestionState::HeavyModerate:
            // Nghẽn nặng ngay từ đầu -> Khởi tạo ở 40% băng thông
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.40);
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            stateName = "HEAVY_MODERATE";
            break;

        case CongestionState::HeavyLight:
            // Chớm nghẽn -> Khởi tạo ở 60% băng thông
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.60);
            m_cooldownUntilMs = ctime + 1500;
            stateName = "HEAVY_LIGHT";
            break;

        case CongestionState::Light:
            // Nhiễu RF ngẫu nhiên hoặc RTT cao nhẹ -> Khởi tạo ở 70% băng thông
            calculatedInitialBitrate = static_cast<unsigned int>(linkCapacityKbps * 0.70);
            stateName = "LIGHT/HOLD";
            break;

        case CongestionState::Clear:
        default:
            // Sóng thông thoáng tuyệt đối -> Khởi tạo ở 80% băng thông tối ưu
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

        qInfo().noquote() << QString("[BelaCoder-SRT] LIVE BOOTSTRAP: Initial Bitrate calculated via Partition State [%1]: %2 kbps (BW: %3 Mbps, RTT: %4ms, Loss: %5)")
                    .arg(stateName)
                    .arg(m_currentBitrateKbps)
                    .arg(smoothedBw, 0, 'f', 2)
                    .arg(smoothedRtt, 0, 'f', 1)
                    .arg(rawLossTotal);

        emit bitrateChanged(m_currentBitrateKbps);
        return;
    }

    // Cập nhật nguyên nhân trạng thái cho Heartbeat Timer
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

    // ── 7.8. Cross-Transport C2 Priority Arbitration ──
    // Kịch bản 4: C2 Offline hoặc Critical -> TẮT VIDEO HOÀN TOÀN để cứu lệnh điều khiển Drone!
    if (!m_isVideoEnabled) {
        if (m_currentBitrateKbps > 0) {
            applyNewBitrate(0, smoothedRtt, smoothedBw, deltaLoss);
        }
        m_consecutiveClearCount = 0;
        m_latestStatusReason = QString("C2_EMERGENCY (Video OFF - Uu tien 100% C2, Mode: %1)").arg(c2PriorityToString(m_c2Priority));
        return;
    }

    // Nếu vừa hồi phục từ C2_ONLY (bitrate = 0), khởi động lại ở mức sàn an toàn
    if (m_currentBitrateKbps == 0) {
        m_currentBitrateKbps = m_minBitrateKbps;
        m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
        applyNewBitrate(m_minBitrateKbps, smoothedRtt, smoothedBw, deltaLoss);
        return;
    }

    // Kịch bản 3: C2 Poor (MAVLink bị trễ / Retransmit tăng) -> KHÔNG TĂNG VIDEO, bóp bitrate video
    if (m_c2Priority == C2PriorityLevel::Critical) {
        m_consecutiveClearCount = 0;
        unsigned int safeC2Bitrate = qMin(m_currentBitrateKbps, 400u);
        if (m_currentBitrateKbps > safeC2Bitrate) {
            applyNewBitrate(safeC2Bitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
        m_latestStatusReason = QString("C2_PROTECTION (C2 suy giam -> Bop video 400k, C2 RTT: %1ms)").arg(m_c2Rtt, 0, 'f', 1);
        return;
    }

    // Kịch bản C2 Fair (High Priority) -> Khóa cổng tăng bitrate của Video để ưu tiên C2
    if (m_c2Priority == C2PriorityLevel::High) {
        m_consecutiveClearCount = 0;
    }

    // ── 8. Decision Logic & Anti-Oscillation (🔴 Mục 1, 2, 3 & 🟢 Mục 9) ──
    qint64 timeSinceLastChange = ctime - m_lastBitrateChangeTime;

    if (state == CongestionState::Panic) {
        // Reset bộ đếm CLEAR & kích hoạt Cooldown 2.0s
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;

        if (timeSinceLastChange >= BITRATE_DECR_FAST_INTERVAL_MS) {
            unsigned int dropAmount = qMax(BITRATE_DECR_MIN_KBPS * 2, static_cast<unsigned int>(m_currentBitrateKbps * 0.35));
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
            unsigned int dropAmount = qMax(BITRATE_DECR_MIN_KBPS, static_cast<unsigned int>(m_currentBitrateKbps * 0.20));
            unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : m_minBitrateKbps;

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateChangeTime = ctime;
            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
    else if (state == CongestionState::HeavyLight) {
        m_consecutiveClearCount = 0;
        m_cooldownUntilMs = ctime + 1500; // 1.5s cooldown

        if (timeSinceLastChange >= BITRATE_DECR_NORMAL_INTERVAL_MS) {
            unsigned int dropAmount = qMax(BITRATE_DECR_MIN_KBPS, static_cast<unsigned int>(m_currentBitrateKbps * 0.10));
            unsigned int targetBitrate = (m_currentBitrateKbps > dropAmount) ? (m_currentBitrateKbps - dropAmount) : m_minBitrateKbps;

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateChangeTime = ctime;
            applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
        }
    }
    else if (state == CongestionState::Light) {
        // Reset bộ đếm CLEAR khi có bất kỳ dấu hiệu chớm nghẽn nào
        m_consecutiveClearCount = 0;
    }
    else if (state == CongestionState::Clear) {
        // Tăng biến đếm số sample CLEAR liên tiếp (🔴 Mục 1)
        m_consecutiveClearCount++;

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
        bool consecutiveClearMet = (m_consecutiveClearCount >= CONSECUTIVE_CLEAR_REQUIRED);

        if (cooldownExpired && decisionIntervalExpired && consecutiveClearMet) {
            // Giới hạn bitrate tăng theo chất lượng đường truyền (tinh chỉnh mịn cho dải vài trăm kbps)
            unsigned int stepKbps = BITRATE_INCR_MIN_KBPS;
            if (rttInflation < 5.0) {
                stepKbps = 50; // Mạng cực kỳ thông thoáng và RTT sát đáy
            } else if (rttInflation < 15.0) {
                stepKbps = 30; // Mạng tốt
            } else {
                stepKbps = 20; // Thăm dò cẩn trọng
            }

            unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;

            // Chỉ giới hạn trần khi có dấu hiệu trễ / nghẽn mạng thực sự
            if (estBwKbps > 0.0 && rttInflation > 60.0 && targetBitrate > estBwKbps * 0.90) {
                targetBitrate = static_cast<unsigned int>(estBwKbps * 0.90);
            }

            targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            targetBitrate = qBound(m_minBitrateKbps, targetBitrate, m_maxBitrateKbps);

            m_lastBitrateIncrTime = ctime;
            m_lastBitrateChangeTime = ctime;
            m_consecutiveClearCount = 0; // Reset để đợi tiếp chu kỳ CLEAR mới

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
    qInfo() << "[BelaCoder-SRT] Emitting bitrateChanged:" << m_currentBitrateKbps << "kbps";
    emit bitrateChanged(m_currentBitrateKbps);
}
