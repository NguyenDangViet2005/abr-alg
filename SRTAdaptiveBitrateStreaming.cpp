#include "SRTAdaptiveBitrateStreaming.h"
#include "ABRConfigs.h"
#include <algorithm>

SRTAdaptiveBitrateStreaming::SRTAdaptiveBitrateStreaming(QObject *parent)
    : IAdaptiveBitrateStreaming(parent)
    , m_isRunning(false)
    , m_currentBitrateKbps(0)
    , m_minBitrateKbps(DEFAULT_MIN_BITRATE_KBPS)
    , m_maxBitrateKbps(DEFAULT_MAX_BITRATE_KBPS)
    , m_c2Quality(C2Quality::Good)
    , m_c2Priority(C2PriorityLevel::Normal)
    , m_isVideoEnabled(true)
    , m_isExplicitC2Only(false)
    , m_isStrictVideoCutoff(ABR_DEFAULT_C2_STRICT_VIDEO_CUTOFF)
    , m_c2Rtt(0.0)
    , m_c2Retransmits(0)
    , m_c2Unacked(0)
    , m_lastC2PacketTime(0)
    , m_rttAvg(0.0)
    , m_rttAvgDelta(0.0)
    , m_prevRtt(MIN_VALID_RTT_MS)
    , m_rttMin(MIN_VALID_RTT_MS)
    , m_rttMinSeeded(false)
    , m_lastBitrateChangeTime(0)
    , m_lastBitrateIncrTime(0)
    , m_cooldownUntilMs(0)
    , m_consecutiveZeroLossCount(0)
    , m_clearSinceMs(0)
    , m_lastCongestedBitrate(0)
    , m_lastProbeTime(0)
    , m_heartbeatTimer(nullptr)
    , m_latestSmoothedRtt(0.0)
    , m_latestSmoothedBw(0.0)
    , m_latestDeltaLoss(0)
    , m_lossPercentAvg(0.0)
    , m_latestSmoothedLossPercent(0.0)
    , m_latestStatusReason("IDLE (Waiting for QoS from Server...)")
    , m_lastQosPacketTime(0)
    , m_lastLossTotal(0)
    , m_hasLastLoss(false)
    , m_lastSentTotal(0)
    , m_hasLastSent(false)
    , m_lastDropSndTotal(0)
    , m_hasLastDropSnd(false)
    , m_lastRetransTotal(0)
    , m_hasLastRetrans(false)
    , m_isBootstrapped(false)
    , m_lastCongestionState(CongestionState::Clear)
    , m_wasCongested(false)
    , m_lastKeyframeRequestTime(0)
    , m_consecutivePanicCount(0)
    , m_congestionStartTimeMs(0)
    , m_consecutiveCongestedSamples(0)
    , m_networkEnvironmentChanged(false)
    , m_lastCameraQosTime(0)
    , m_lastActiveTrafficTime(0)
    , m_isTrafficIdle(true)
    , m_isVideoCollapsed(false)
    , m_needRecoveryRefresh(false)
    , m_lastCollapseLogTime(0)
    , m_lastAchievedBitrateKbps(0)
    , m_smoothedRssi(-100.0)
    , m_smoothedSignal(0)
    , m_lastSiyiPacketTime(0)
    , m_currentEnvCeiling(DEFAULT_MAX_BITRATE_KBPS)
    , m_lastEnvChangeRssi(-60.0)
    , m_currentEnvName("LOS/Ultra (1080p60)")
    , m_hasSiyiTelemetry(false)
{
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &SRTAdaptiveBitrateStreaming::onHeartbeatTimeout);

    qInfo() << "[QoS Engine] Initialized ABR service. Default Bitrate:" << m_currentBitrateKbps << "kbps";
}

void SRTAdaptiveBitrateStreaming::start()
{
    m_isRunning = true;
    m_currentBitrateKbps = 0;
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_rttWarmupHistory.clear();
    m_siyiRssiHistory.clear();

    m_smoothedRssi = -100.0;
    m_smoothedSignal = 0;
    m_lastSiyiPacketTime = 0;
    m_currentEnvCeiling = DEFAULT_MAX_BITRATE_KBPS;
    m_lastEnvChangeRssi = -60.0;
    m_currentEnvName = "LOS/Ultra (1080p60)";
    m_hasSiyiTelemetry = false;

    m_isVideoEnabled = true;
    m_isExplicitC2Only = false;
    m_clearSinceMs = 0;
    m_lastCongestedBitrate = 0;
    m_c2Quality = C2Quality::Good;
    m_c2Priority = C2PriorityLevel::Normal;
    m_c2Rtt = 0.0;
    m_c2Retransmits = 0;
    m_c2Unacked = 0;
    m_lastC2PacketTime = 0;

    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = MIN_VALID_RTT_MS;
    m_rttMin = MIN_VALID_RTT_MS;
    m_rttMinSeeded = false;

    m_lastBitrateChangeTime = QDateTime::currentMSecsSinceEpoch();
    m_lastBitrateIncrTime = m_lastBitrateChangeTime;
    m_cooldownUntilMs = 0;
    m_consecutiveZeroLossCount = 0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_hasLastSent = false;
    m_lastSentTotal = 0;
    m_hasLastDropSnd = false;
    m_lastDropSndTotal = 0;
    m_hasLastDropRcv = false;
    m_lastDropRcvTotal = 0;
    m_hasLastRetrans = false;
    m_lastRetransTotal = 0;
    m_isBootstrapped = false;
    m_lastCongestionState = CongestionState::Clear;
    m_wasCongested = false;
    m_lastKeyframeRequestTime = 0;
    m_consecutivePanicCount = 0;
    m_lastCameraQosTime = 0;
    m_lastActiveTrafficTime = 0;
    m_isTrafficIdle = true;
    m_isVideoCollapsed = false;
    m_needRecoveryRefresh = false;
    m_lastCollapseLogTime = 0;
    m_lastAchievedBitrateKbps = 0;
    m_latestStatusReason = "IDLE (Waiting for QoS from Server...)";
    m_lastQosPacketTime = 0;
    m_lossPercentAvg = 0.0;
    m_latestSmoothedLossPercent = 0.0;

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
    m_rttWarmupHistory.clear();

    m_rttAvg = 0.0;
    m_rttAvgDelta = 0.0;
    m_prevRtt = MIN_VALID_RTT_MS;
    m_rttMin = MIN_VALID_RTT_MS;
    m_rttMinSeeded = false;
    m_lastBitrateChangeTime = QDateTime::currentMSecsSinceEpoch();
    m_lastBitrateIncrTime = m_lastBitrateChangeTime;
    m_cooldownUntilMs = 0;
    m_consecutiveZeroLossCount = 0;
    m_clearSinceMs = 0;
    m_previousStableBitrate = 0;
    m_lastCongestedBitrate = 0;
    m_lastProbeTime = 0;
    m_bottleneckBwMbps = 0.0;
    m_hasLastLoss = false;
    m_lastLossTotal = 0;
    m_hasLastSent = false;
    m_lastSentTotal = 0;
    m_hasLastDropSnd = false;
    m_lastDropSndTotal = 0;
    m_hasLastDropRcv = false;
    m_lastDropRcvTotal = 0;
    m_hasLastRetrans = false;
    m_lastRetransTotal = 0;
    m_lastCongestionState = CongestionState::Clear;
    m_wasCongested = false;
    m_lastKeyframeRequestTime = 0;
    m_consecutivePanicCount = 0;
    m_congestionStartTimeMs = 0;
    m_consecutiveCongestedSamples = 0;
    m_networkEnvironmentChanged = false;
    m_lastCameraQosTime = 0;
    m_lastActiveTrafficTime = 0;
    m_isTrafficIdle = true;
    m_isVideoCollapsed = false;
    m_needRecoveryRefresh = false;
    m_lastCollapseLogTime = 0;
    m_lastAchievedBitrateKbps = 0;
    m_isBootstrapped = false;
    m_lastQosPacketTime = 0;
    m_latestStatusReason = "IDLE (Waiting for QoS from Server...)";
    m_latestSmoothedRtt = 0.0;
    m_latestSmoothedBw = 0.0;
    m_latestDeltaLoss = 0;
    m_lossPercentAvg = 0.0;
    m_latestSmoothedLossPercent = 0.0;
    m_isExplicitC2Only = false;
    m_c2Quality = C2Quality::Good;
    m_c2Priority = C2PriorityLevel::Normal;
    m_c2Rtt = 0.0;
    m_c2Retransmits = 0;
    m_c2Unacked = 0;
    m_lastC2PacketTime = 0;
    m_isVideoEnabled = true;

    m_siyiRssiHistory.clear();
    m_smoothedRssi = -100.0;
    m_smoothedSignal = 0;
    m_lastSiyiPacketTime = 0;
    m_currentEnvCeiling = DEFAULT_MAX_BITRATE_KBPS;
    m_lastEnvChangeRssi = -60.0;
    m_currentEnvName = "LOS/Ultra (1080p60)";
    m_hasSiyiTelemetry = false;

    qInfo() << "[BelaCoder-SRT] Reset bitrate to:" << m_currentBitrateKbps << "kbps";
    emit bitrateChanged(m_currentBitrateKbps);
}

void SRTAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{
    if (newMaxAbrBitrate < MIN_ACTIVE_VIDEO_BITRATE_KBPS) {
        qWarning() << "[BelaCoder-SRT] Rejected Max Bitrate (below survival floor):"
                   << newMaxAbrBitrate << "kbps";
        return;
    }

    m_maxBitrateKbps = newMaxAbrBitrate;
    if (m_currentBitrateKbps > m_maxBitrateKbps) {
        m_currentBitrateKbps = (m_maxBitrateKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
        emit bitrateChanged(m_currentBitrateKbps);
    }
    qInfo() << "[BelaCoder-SRT] Updated Max Bitrate:" << m_maxBitrateKbps << "kbps";
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

    m_isVideoEnabled = m_isStrictVideoCutoff ? !m_isExplicitC2Only : true;

    if (m_isExplicitC2Only) {
        m_c2Quality = C2Quality::Critical;
        m_c2Priority = C2PriorityLevel::C2_Only;
    }
    else {
        if (m_lastC2PacketTime > 0 && (now - m_lastC2PacketTime) > 5000) {
            m_c2Quality = C2Quality::Offline;
            m_c2Priority = C2PriorityLevel::Normal;
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

    const unsigned int c2PolicyTarget = (m_isExplicitC2Only && m_isStrictVideoCutoff)
                                            ? VIDEO_DISABLED_BITRATE_KBPS
                                            : MIN_ACTIVE_VIDEO_BITRATE_KBPS;
    const bool c2NeedsSqueeze = m_isExplicitC2Only || (m_currentBitrateKbps == VIDEO_DISABLED_BITRATE_KBPS);
    if (c2NeedsSqueeze && m_currentBitrateKbps != c2PolicyTarget) {
        qWarning().noquote() << QString("[C2 Priority] Squeezing video to %1 kbps (policy: %2)")
                                    .arg(c2PolicyTarget)
                                    .arg(m_isStrictVideoCutoff ? "STRICT_CUTOFF" : "SURVIVAL_FLOOR");
        applyNewBitrate(c2PolicyTarget, m_c2Rtt, 0.0, 0);
    }

    if (m_c2Priority != prevPriority) {
        emit c2PriorityChanged(static_cast<int>(m_c2Priority), c2PriorityToString(m_c2Priority));
    }
}

void SRTAdaptiveBitrateStreaming::handleC2ConnectionStats(const QVector<SRTPeerStat> &peers)
{
    if (!m_isRunning || peers.isEmpty()) return;

    double worstRtt = 0.0;
    int worstRetrans = 0;
    int worstUnacked = 0;
    bool explicitC2Only = false;

    for (const SRTPeerStat &p : peers) {
        worstRtt = qMax(worstRtt, p.msRTT);
        worstRetrans = qMax(worstRetrans, p.pktRetransTotal);
        worstUnacked = qMax(worstUnacked, p.pktSndDropTotal);
        explicitC2Only = explicitC2Only || p.isC2Only;
    }

    m_c2Rtt = worstRtt;
    m_c2Retransmits = worstRetrans;
    m_c2Unacked = worstUnacked;
    m_isExplicitC2Only = explicitC2Only;
    m_lastC2PacketTime = QDateTime::currentMSecsSinceEpoch();

    evaluateC2Quality();
}


void SRTAdaptiveBitrateStreaming::onHeartbeatTimeout()
{
    if (!m_isRunning) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool hasRecentQos = (m_lastCameraQosTime > 0 &&
                         (now - m_lastCameraQosTime) < CAMERA_QOS_STALE_TIMEOUT_MS);

    if (!hasRecentQos) {
        if (m_lastCameraQosTime > 0) {
            if (now - m_lastCollapseLogTime >= 5000) {
                m_lastCollapseLogTime = now;
                qWarning().noquote() << QString(">>> [QoS WATCHDOG] ⚠️ LOST CAMERA QoS %1s (prefix 9990)! "
                                                "Stream considered COLLAPSED, forcing floor %2 kbps <<<")
                                            .arg((now - m_lastCameraQosTime) / 1000.0, 0, 'f', 1)
                                            .arg(MIN_ACTIVE_VIDEO_BITRATE_KBPS);
            }
            m_isVideoCollapsed = true;
            m_needRecoveryRefresh = true;
            if (m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS) {
                applyNewBitrate(MIN_ACTIVE_VIDEO_BITRATE_KBPS, m_latestSmoothedRtt,
                                m_latestSmoothedBw, 0);
            }
        } else {
            qInfo().noquote() << "[QoS] Waiting for QoS data...";
        }
        return;
    }

    evaluateC2Quality();

    if (m_lastAchievedBitrateKbps > 0 && m_currentBitrateKbps > 0) {
        const double ratio = static_cast<double>(m_lastAchievedBitrateKbps) /
                             static_cast<double>(m_currentBitrateKbps);
        if (ratio < 0.25) {
            if (!m_isVideoCollapsed) {
                m_isVideoCollapsed = true;
            }
        } else if (m_isVideoCollapsed) {
            m_isVideoCollapsed = false;
        }
    }

    QString videoStr;
    QString videoStateLabel;
    if (!m_isVideoEnabled) {
        videoStr = "OFF";
        videoStateLabel = "OFF";
    } else if (m_isTrafficIdle) {
        videoStateLabel = "\033[1;33mIDLE\033[0m";
        videoStr = QString("\033[1;33m%1 kbps (IDLE)\033[0m").arg(m_currentBitrateKbps);
    } else if (m_isVideoCollapsed || m_lastCongestionState == CongestionState::Panic) {
        videoStateLabel = "\033[1;31mCOLLAPSED\033[0m";
        videoStr = QString("\033[1;31m%1 kbps (COLLAPSED)\033[0m").arg(m_currentBitrateKbps);
    } else {
        videoStateLabel = "ON";
        videoStr = QString("\033[1;32m%1 kbps\033[0m").arg(m_currentBitrateKbps);
    }

    const QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString siyiLabel = m_hasSiyiTelemetry
                            ? QString("SIYI: %1dBm (%2% | %3)").arg(m_smoothedRssi, 0, 'f', 0).arg(m_smoothedSignal).arg(m_currentEnvName)
                            : "SIYI: N/A";

    qInfo().noquote() << QString("[%1] RTT: %2ms | BW: %3 Mbps | Loss: %4% | %5 | Video: %6 | State: %7 --> bitrate: %8")
        .arg(timeStr)
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestSmoothedLossPercent, 0, 'f', 1)
        .arg(siyiLabel)
        .arg(videoStateLabel)
        .arg(m_latestStatusReason)
        .arg(videoStr);
}

void SRTAdaptiveBitrateStreaming::handleCameraReportedBitrate(int achievedKbps)
{
    if (!m_isRunning) return;
    m_lastAchievedBitrateKbps = (achievedKbps > 0) ? static_cast<unsigned int>(achievedKbps) : 0;
}

const RfEnvironment& SRTAdaptiveBitrateStreaming::getRfEnvironment(double rssi) const
{
    for (int i = 0; i < RF_ENVIRONMENT_COUNT; ++i) {
        if (rssi >= RF_ENVIRONMENTS[i].minRssi) {
            return RF_ENVIRONMENTS[i];
        }
    }
    return RF_ENVIRONMENTS[RF_ENVIRONMENT_COUNT - 1];
}

void SRTAdaptiveBitrateStreaming::handleSiyiLinkStat(const SiyiLinkStat &stat)
{
    if (!m_isRunning || !stat.isAlive) return;

    m_siyiStat = stat;
    m_hasSiyiTelemetry = true;
    m_lastSiyiPacketTime = QDateTime::currentMSecsSinceEpoch();

    // Sliding window median filter cho RSSI (5 mẫu) để lọc nhiễu tức thời
    m_siyiRssiHistory.append(stat.rssi);
    if (m_siyiRssiHistory.size() > 5) {
        m_siyiRssiHistory.removeFirst();
    }
    m_smoothedRssi = calculateMedian(m_siyiRssiHistory);
    m_smoothedSignal = stat.signal;

    const RfEnvironment &env = getRfEnvironment(m_smoothedRssi);

    // Kiểm tra đổi môi trường với ngưỡng trễ Hysteresis (>= 2 dBm) chống rung lắc ranh giới
    bool isCeilingDifferent = (env.ceilingKbps != m_currentEnvCeiling);
    bool exceedsHysteresis = (qAbs(m_smoothedRssi - m_lastEnvChangeRssi) >= RF_ENV_HYSTERESIS_DBM);

    if (isCeilingDifferent && exceedsHysteresis) {
        unsigned int oldCeiling = m_currentEnvCeiling;
        m_currentEnvCeiling = env.ceilingKbps;
        m_lastEnvChangeRssi = m_smoothedRssi;
        m_currentEnvName = env.name;

        if (m_currentEnvCeiling > oldCeiling) {
            // Môi trường mạng MẠNH HƠN (bay lại gần GCS): Nâng trần ngay lập tức, không đợi 30s dò dẫm!
            qInfo().noquote() << QString("[RF Environment] 📶 Entered STRONGER environment: %1 | New Ceiling: %2 kbps (RSSI: %3 dBm)")
                                        .arg(m_currentEnvName).arg(m_currentEnvCeiling).arg(m_smoothedRssi, 0, 'f', 1);

            m_lastCongestedBitrate = m_currentEnvCeiling;
            m_clearSinceMs = m_lastSiyiPacketTime;
            m_lastBitrateIncrTime = m_lastSiyiPacketTime;
            m_networkEnvironmentChanged = true;
        } else {
            // Môi trường mạng YẾU HƠN (bay ra xa GCS / khuất sóng): Khóa trần thấp hơn, chủ động hạ bitrate xuống trước khi rớt gói
            qWarning().noquote() << QString("[RF Environment] ⚠️ Entered WEAKER environment: %1 | Lowering Ceiling: %2 kbps (RSSI: %3 dBm)")
                                        .arg(m_currentEnvName).arg(m_currentEnvCeiling).arg(m_smoothedRssi, 0, 'f', 1);

            m_lastCongestedBitrate = m_currentEnvCeiling;

            if (m_currentBitrateKbps > m_currentEnvCeiling) {
                qInfo().noquote() << QString("[RF Environment] Proactively stepping down bitrate from %1 kbps to %2 kbps for link safety")
                                             .arg(m_currentBitrateKbps).arg(m_currentEnvCeiling);
                applyNewBitrate(m_currentEnvCeiling, m_latestSmoothedRtt, m_latestSmoothedBw, 0);
            }
        }
    }
}

void SRTAdaptiveBitrateStreaming::handleSiyiLinkLost()
{
    if (m_hasSiyiTelemetry) {
        m_hasSiyiTelemetry = false;
        m_siyiStat.isAlive = false;
        qWarning().noquote() << "[RF Environment] ⚠️ SIYI Telemetry lost (> 2.5s). Reverting to transport-only SRT QoS.";
    }
}

void SRTAdaptiveBitrateStreaming::handleQosCameraConnection(const QVector<SRTPeerStat> &peers)
{
    if (!m_isRunning || peers.isEmpty()) {
        return;
    }

    double worstRtt = 0.0;
    double totalBandwidth = 0.0;
    double maxSendRate = 0.0;
    int totalLoss = 0;
    qint64 totalSent = 0;
    int totalDropSnd = 0;
    int totalDropRcv = 0;
    int totalRetrans = 0;

    m_lastCameraQosTime = QDateTime::currentMSecsSinceEpoch();

    for (const SRTPeerStat &peer : peers) {
        worstRtt = qMax(worstRtt, peer.msRTT);
        if (peer.mbpsBandwidth > 0.0) {
            totalBandwidth += peer.mbpsBandwidth;
        }
        maxSendRate = qMax(maxSendRate, peer.mbpsSendRate);
        totalLoss += peer.pktSndLossTotal;
        totalSent += peer.pktSentTotal;
        totalDropSnd += peer.pktSndDropTotal;
        totalDropRcv += peer.pktRcvDropTotal;
        totalRetrans += peer.pktRetransTotal;
    }

    processSrtQos(worstRtt, totalBandwidth, maxSendRate, totalLoss, totalSent,
                  totalDropSnd, totalDropRcv, totalRetrans);
}

unsigned int SRTAdaptiveBitrateStreaming::calculateNetworkPartitionCeiling(double smoothedBw, double lossPercent, CongestionState state) const
{
    Q_UNUSED(lossPercent);
    Q_UNUSED(state);
    unsigned int partitionCeiling = m_maxBitrateKbps;

    // 1. Bandwidth capacity partition (dựa trên ước lượng băng thông SRT):
    double effectiveBw = (m_bottleneckBwMbps > 0.0 && state != CongestionState::Clear) ? m_bottleneckBwMbps : smoothedBw;
    if (effectiveBw > 0.1 && effectiveBw < 150.0) {
        unsigned int bwCap = static_cast<unsigned int>(effectiveBw * 1000.0 * BW_UTILIZATION_RATIO);
        bwCap = qMax(MIN_ACTIVE_VIDEO_BITRATE_KBPS, bwCap);
        partitionCeiling = qMin(partitionCeiling, bwCap);
    }

    // 2. Physical RF environment ceiling (dựa trên môi trường sóng SIYI HM30):
    if (m_hasSiyiTelemetry && m_siyiStat.isAlive && m_currentEnvCeiling > 0) {
        partitionCeiling = qMin(partitionCeiling, m_currentEnvCeiling);
    }

    partitionCeiling = (partitionCeiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
    return qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, partitionCeiling, m_maxBitrateKbps);
}

SRTAdaptiveBitrateStreaming::CongestionState SRTAdaptiveBitrateStreaming::classifyCongestion(
    double lossPercent, double rtt, double rttInflation, bool useExitThresholds, bool hasLatencyDrops, bool isKeyframeBurst) const
{
    const double f = useExitThresholds ? 0.85 : 1.0;
    const double rttScale   = useExitThresholds ? 0.88 : 1.0;
    const double inflScale  = useExitThresholds ? 0.85 : 1.0;

    const double relInflation = (m_rttMin > 0.0) ? (rttInflation / m_rttMin) : 0.0;
    const bool isLowLatencyLink = (rtt <= 25.0 && rttInflation < 10.0 && !hasLatencyDrops);
    const double panicLossThreshold = isLowLatencyLink ? (28.0 * f) : (18.0 * f);

    // Large keyframe I-frames can temporarily bloat queue latency for up to 800ms.
    // Unless loss is high (lossPercent >= 10%), suppress false Panic triggers from transient RTT spikes.
    const bool allowRttPanic = !isKeyframeBurst || (lossPercent >= 10.0 * f);

    if (lossPercent >= panicLossThreshold || 
        (hasLatencyDrops && lossPercent >= 8.0 * f) || 
        (allowRttPanic && (
            (rtt >= 260.0 * rttScale && rttInflation > 160.0 * inflScale) ||
            (rttInflation >= 150.0 * inflScale) ||
            (rtt >= 200.0 * rttScale && rttInflation >= 100.0 * inflScale && relInflation >= 3.0 * inflScale)
        ))) {
        return CongestionState::Panic;
    }
    if (lossPercent >= 8.0 * f || 
        (rtt >= 140.0 * rttScale && rttInflation > 60.0 * inflScale) ||
        (rttInflation >= 90.0 * inflScale) ||
        (rtt >= 120.0 * rttScale && rttInflation >= 50.0 * inflScale && relInflation >= 2.0 * inflScale)) {
        return CongestionState::Severe;
    }
    if (lossPercent >= 2.5 * f || 
        (rtt >= 70.0 * rttScale && rttInflation > 30.0 * inflScale) ||
        (rttInflation >= 30.0 * inflScale) ||
        (m_rttMin > 0.0 && ((rttInflation >= 12.0 * inflScale && relInflation >= 2.0 * inflScale) ||
                            (rttInflation >= 20.0 * inflScale && relInflation >= 1.5 * inflScale)))) {
        return CongestionState::Moderate;
    }
    return CongestionState::Clear;
}

void SRTAdaptiveBitrateStreaming::processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps,
                                                int rawLossTotal, qint64 rawSentTotal,
                                                int rawDropSndTotal, int rawDropRcvTotal,
                                                int rawRetransTotal)
{
    qint64 ctime = QDateTime::currentMSecsSinceEpoch();

    bool isStreamRecovering = m_needRecoveryRefresh || (m_lastQosPacketTime > 0 && (ctime - m_lastQosPacketTime) >= CAMERA_QOS_STALE_TIMEOUT_MS);
    if (isStreamRecovering) {
        m_needRecoveryRefresh = false;
        m_isVideoCollapsed = false;
        m_consecutivePanicCount = 0;

        m_rttHistory.clear();
        m_bwHistory.clear();
        m_rttWarmupHistory.clear();

        m_hasLastLoss = false;
        m_hasLastSent = false;
        m_hasLastDropSnd = false;
        m_hasLastDropRcv = false;
        m_hasLastRetrans = false;

        m_lossPercentAvg = 0.0;
        m_latestSmoothedLossPercent = 0.0;
        m_consecutiveZeroLossCount = 0;

        m_lastKeyframeRequestTime = ctime;
        m_cooldownUntilMs = ctime + 800;
        emit requestKeyframe();
    }

    // Sliding Window Smoothing
    if (rawRtt > 0.0) {
        m_rttHistory.append(rawRtt);
        if (m_rttHistory.size() > SLIDING_WINDOW_SIZE) m_rttHistory.removeFirst();
    }
    if (rawBandwidthMbps > 0.0 && rawBandwidthMbps <= 150.0) {
        if (rawBandwidthMbps <= 2.5 && rawBandwidthMbps >= 0.1) {
            // A measurement <= 2.5 Mbps is a true bottleneck reading (token-bucket depleted)
            m_bottleneckBwMbps = (m_bottleneckBwMbps > 0.0)
                                     ? qMin(m_bottleneckBwMbps, rawBandwidthMbps)
                                     : rawBandwidthMbps;
        } else if (rawBandwidthMbps > 2.5) {
            // Bandwidth reading clearly exceeds 2.5 Mbps: bottleneck is cleared
            m_bottleneckBwMbps = 0.0;
        }
        m_bwHistory.append(rawBandwidthMbps);
        if (m_bwHistory.size() > SLIDING_WINDOW_SIZE) m_bwHistory.removeFirst();
    }

    double smoothedRtt = (m_rttHistory.isEmpty()) ? rawRtt : calculateMedian(m_rttHistory);
    double smoothedBw = (m_bwHistory.isEmpty()) 
                            ? ((rawBandwidthMbps > 0.0 && rawBandwidthMbps <= 150.0) 
                                   ? rawBandwidthMbps 
                                   : 0.0)
                            : calculateAverage(m_bwHistory);

    if (smoothedRtt < MIN_VALID_RTT_MS) {
        smoothedRtt = MIN_VALID_RTT_MS;
    }

    double dt = (m_lastQosPacketTime > 0) ? (ctime - m_lastQosPacketTime) / 1000.0 : 0.25;
    bool isStaleSample = (dt > 2.0);
    if (isStaleSample) {
        dt = 0.5;
    }
    if (dt < 0.05) dt = 0.05;

    // Update RTT Statistics & Thresholds
    if (m_rttAvg == 0.0) {
        m_rttAvg = smoothedRtt;
    } else {
        m_rttAvg = m_rttAvg * 0.95 + 0.05 * smoothedRtt;
    }

    double delta_rtt = smoothedRtt - m_prevRtt;
    m_rttAvgDelta = m_rttAvgDelta * 0.8 + delta_rtt * 0.2;
    m_prevRtt = smoothedRtt;

    if (!m_rttMinSeeded) {
        if (smoothedRtt >= MIN_VALID_RTT_MS) {
            m_rttWarmupHistory.append(smoothedRtt);
            m_rttMin = calculateMedian(m_rttWarmupHistory);
        }
        if (m_rttWarmupHistory.size() >= RTT_BASELINE_WARMUP_SAMPLES) {
            m_rttMinSeeded = true;
        }
    } else {
        m_rttMin *= (1.0 + RTT_BASELINE_DRIFT_PER_SEC * dt);
        if (smoothedRtt >= MIN_VALID_RTT_MS && smoothedRtt < m_rttMin && m_rttAvgDelta < 0.5) {
            m_rttMin = smoothedRtt;
        }
    }
    if (m_rttMin < MIN_VALID_RTT_MS) {
        m_rttMin = MIN_VALID_RTT_MS;
    }

    auto deltaCounter = [](qint64 raw, qint64 last, bool hasLast, bool &resetOut) {
        resetOut = false;
        if (!hasLast) return qint64(0);
        if (raw >= last) return raw - last;
        resetOut = true;
        return qint64(0);
    };

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

    bool lossReset = false, dropSndReset = false, dropRcvReset = false, retransReset = false;
    const qint64 deltaLossRaw = deltaCounter(rawLossTotal, m_lastLossTotal, m_hasLastLoss, lossReset);
    const qint64 deltaDropSnd = deltaCounter(rawDropSndTotal, m_lastDropSndTotal, m_hasLastDropSnd, dropSndReset);
    const qint64 deltaDropRcv = deltaCounter(rawDropRcvTotal, m_lastDropRcvTotal, m_hasLastDropRcv, dropRcvReset);
    const qint64 deltaRetrans = deltaCounter(rawRetransTotal, m_lastRetransTotal, m_hasLastRetrans, retransReset);

    m_lastLossTotal = rawLossTotal;
    m_hasLastLoss = true;
    m_lastDropSndTotal = rawDropSndTotal;
    m_hasLastDropSnd = true;
    m_lastDropRcvTotal = rawDropRcvTotal;
    m_hasLastDropRcv = true;
    m_lastRetransTotal = rawRetransTotal;
    m_hasLastRetrans = true;

    const int deltaLoss = static_cast<int>(deltaLossRaw);

    const bool hasActiveTraffic = (deltaSent > 0 || rawSendRateMbps >= 0.02);
    if (hasActiveTraffic) {
        m_lastActiveTrafficTime = ctime;
        if (m_isTrafficIdle) {
            m_isTrafficIdle = false;
            m_hasLastLoss = false;
            m_hasLastSent = false;
            m_hasLastDropSnd = false;
            m_hasLastDropRcv = false;
            m_hasLastRetrans = false;
            m_lossPercentAvg = 0.0;
            m_latestSmoothedLossPercent = 0.0;
            m_consecutiveZeroLossCount = 0;
            emit trafficActiveChanged(true);
        }
    } else {
        if (m_lastActiveTrafficTime > 0 && (ctime - m_lastActiveTrafficTime >= TRAFFIC_IDLE_TIMEOUT_MS)) {
            if (!m_isTrafficIdle) {
                m_isTrafficIdle = true;
                emit trafficActiveChanged(false);
            }
        }
    }

    if (m_isTrafficIdle) {
        m_latestSmoothedRtt = smoothedRtt;
        m_latestSmoothedBw = smoothedBw;
        m_latestDeltaLoss = 0;
        m_lastQosPacketTime = ctime;
        m_latestStatusReason = "IDLE (Camera off / No traffic)";
        return;
    }

    const qint64 wireLostPkts = qMax(deltaLossRaw, deltaRetrans);
    const qint64 deltaFailures = wireLostPkts + deltaDropSnd + deltaDropRcv;

    double instantLossPercent = 0.0;
    if (deltaFailures > 0) {
        if (deltaSent > 0) {
            // Minimum sample floor of 20 packets to avoid small-sample noise at low bitrates
            // (e.g. losing 2 out of 8 packets won't artificially blow up to 25% causing false Panic)
            const qint64 sampleBase = qMax(deltaSent, MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE);
            instantLossPercent = (static_cast<double>(deltaFailures) / static_cast<double>(sampleBase)) * 100.0;
        } else {
            double currentRateMbps = (rawSendRateMbps > 0.1) ? rawSendRateMbps : (static_cast<double>(m_currentBitrateKbps) / 1000.0);
            double expectedPkts = (currentRateMbps * 1000000.0 * dt) / (1316.0 * 8.0);
            if (expectedPkts < static_cast<double>(MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE)) {
                expectedPkts = static_cast<double>(MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE);
            }
            instantLossPercent = (static_cast<double>(deltaFailures) / expectedPkts) * 100.0;
        }
    }
    instantLossPercent = qBound(0.0, instantLossPercent, 100.0);

    if (deltaFailures == 0) {
        m_consecutiveZeroLossCount++;
        if (m_consecutiveZeroLossCount >= 3) {
            m_lossPercentAvg = 0.0;
        } else if (m_consecutiveZeroLossCount >= 2) {
            m_lossPercentAvg *= 0.25;
            if (m_lossPercentAvg < 1.0) {
                m_lossPercentAvg = 0.0;
            }
        } else {
            m_lossPercentAvg *= 0.50;
            if (m_lossPercentAvg < 1.0) {
                m_lossPercentAvg = 0.0;
            }
        }
    } else {
        m_consecutiveZeroLossCount = 0;
        if (instantLossPercent > m_lossPercentAvg) {
            m_lossPercentAvg = (m_lossPercentAvg * 0.55) + (instantLossPercent * 0.45);
        } else {
            m_lossPercentAvg = (m_lossPercentAvg * 0.70) + (instantLossPercent * 0.30);
        }
    }
    m_latestSmoothedLossPercent = m_lossPercentAvg;

    if (deltaFailures == 0 && m_lossPercentAvg < 1.0 && smoothedRtt > m_rttMin) {
        m_rttMin = (m_rttMin * 0.998) + (smoothedRtt * 0.002);
    }

    // Calculate Dynamic Metrics
    double rttInflation = (smoothedRtt > m_rttMin) ? (smoothedRtt - m_rttMin) : 0.0;
    const bool hasLatencyDrops = (deltaDropSnd > 0 || deltaDropRcv > 0);
    const bool isKeyframeBurst = (ctime - m_lastKeyframeRequestTime < KEYFRAME_IMMUNITY_DURATION_MS);

    const int currentSeverity = static_cast<int>(m_lastCongestionState);
    const int enterSeverity = static_cast<int>(
        classifyCongestion(m_lossPercentAvg, smoothedRtt, rttInflation, false, hasLatencyDrops, isKeyframeBurst));
    const int exitSeverity = static_cast<int>(
        classifyCongestion(m_lossPercentAvg, smoothedRtt, rttInflation, true, hasLatencyDrops, isKeyframeBurst));

    CongestionState state;
    if (enterSeverity > currentSeverity) {
        state = static_cast<CongestionState>(enterSeverity);
    } else if (exitSeverity < currentSeverity) {
        state = static_cast<CongestionState>(exitSeverity);
    } else {
        state = m_lastCongestionState;
    }

    m_lastCongestionState = state;
    m_latestSmoothedRtt = smoothedRtt;
    m_latestSmoothedBw = smoothedBw;
    m_latestDeltaLoss = static_cast<int>(deltaFailures);
    m_lastQosPacketTime = ctime;

    if (state == CongestionState::Panic) {
        if (ctime - m_lastCollapseLogTime > 2000) {
            m_lastCollapseLogTime = ctime;
        }
    }

    if (isStaleSample) {
        return;
    }

    unsigned int bandwidthCapKbps = calculateNetworkPartitionCeiling(smoothedBw, m_lossPercentAvg, state);

    if (m_lossPercentAvg >= 2.0) {
        double currentRateMbps = (rawSendRateMbps > 0.2) 
                                     ? rawSendRateMbps 
                                     : (static_cast<double>(m_currentBitrateKbps) / 1000.0);
        double lossFraction = qBound(0.0, m_lossPercentAvg / 100.0, 0.95);
        double goodputMbps = currentRateMbps * (1.0 - lossFraction);
        double safeCapKbps = goodputMbps * 1000.0 * 0.85;

        unsigned int estimatedCap = static_cast<unsigned int>(qMax(static_cast<double>(MIN_ACTIVE_VIDEO_BITRATE_KBPS), safeCapKbps));
        estimatedCap = (estimatedCap / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
        bandwidthCapKbps = qMin(bandwidthCapKbps, estimatedCap);

        if (smoothedBw > goodputMbps || smoothedBw <= 0.0) {
            smoothedBw = goodputMbps;
            m_latestSmoothedBw = smoothedBw;
        }
    }
    bandwidthCapKbps = (bandwidthCapKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
    bandwidthCapKbps = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, bandwidthCapKbps, m_maxBitrateKbps);

    // Live Bootstrapping: initial bitrate starts low and ramps smoothly
    if (!m_isBootstrapped) {
        unsigned int calculatedInitialBitrate = (state == CongestionState::Panic)
            ? MIN_ACTIVE_VIDEO_BITRATE_KBPS
            : 100u;

        calculatedInitialBitrate = qMin(calculatedInitialBitrate, bandwidthCapKbps);
        calculatedInitialBitrate = (calculatedInitialBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
        calculatedInitialBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, calculatedInitialBitrate, m_maxBitrateKbps);

        m_isBootstrapped = true;
        m_lastBitrateChangeTime = ctime;
        m_lastBitrateIncrTime = ctime;
        m_currentBitrateKbps = 0;

        applyNewBitrate(calculatedInitialBitrate, smoothedRtt, smoothedBw, deltaLoss);
        return;
    }

    // Status reason
    if (ctime < m_cooldownUntilMs && state != CongestionState::Clear) {
        double remSec = (m_cooldownUntilMs - ctime) / 1000.0;
        m_latestStatusReason = QString("COOLDOWN (Con %1s)").arg(remSec, 0, 'f', 1);
    } else if (state == CongestionState::Clear) {
        m_latestStatusReason = "CLEAR (1080p)";
    } else if (state == CongestionState::Moderate) {
        m_latestStatusReason = "MODERATE (720p 2.2M)";
    } else if (state == CongestionState::Severe) {
        m_latestStatusReason = "SEVERE (480p 1.0M)";
    } else {
        m_latestStatusReason = "\033[1;31mPANIC (Survival 50k)\033[0m";
    }

    qint64 timeSinceLastChange = ctime - m_lastBitrateChangeTime;

    unsigned int targetProfileBitrate = m_maxBitrateKbps;
    qint64 requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;

    switch (state) {
    case CongestionState::Panic:
        targetProfileBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::Severe:
        targetProfileBitrate = 1000;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::Moderate:
        targetProfileBitrate = 2200;
        requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;
        break;
    case CongestionState::Clear:
    default:
        targetProfileBitrate = m_maxBitrateKbps;
        requiredInterval = BITRATE_INCR_DECISION_INTERVAL_MS;
        break;
    }

    if (state == CongestionState::Clear) {
        m_consecutivePanicCount = 0;
        m_congestionStartTimeMs = 0;
        m_consecutiveCongestedSamples = 0;

        if (m_clearSinceMs == 0) {
            m_clearSinceMs = ctime;
        }

        if (m_wasCongested) {
            m_wasCongested = false;
            if (ctime - m_lastKeyframeRequestTime >= KEYFRAME_REQUEST_COOLDOWN_MS) {
                m_lastKeyframeRequestTime = ctime;
                emit requestKeyframe();
            }
        }

        bool isBufferDrained = (rttInflation < 8.0 || (m_rttMin > 0.0 && smoothedRtt <= m_rttMin * 1.30 + 5.0));

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);

        // Dynamic decision interval in Clear state:
        // When there is huge available headroom (e.g. limit raised), use 1000ms for agile ramp-up.
        // When nearing operating limits, use standard 2000ms for rock-solid stability.
        qint64 availableHeadroom = (bandwidthCapKbps > m_currentBitrateKbps) ? (bandwidthCapKbps - m_currentBitrateKbps) : 0;
        qint64 incrDecisionInterval = (availableHeadroom >= 400 && m_lossPercentAvg == 0.0 && rttInflation < 3.0)
                                          ? 1000
                                          : BITRATE_INCR_DECISION_INTERVAL_MS;
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= incrDecisionInterval);
        bool clearStableMet = (ctime - m_clearSinceMs >= CLEAR_STABLE_DURATION_MS);

        if (cooldownExpired && decisionIntervalExpired && clearStableMet) {
            if (!isBufferDrained) {
                m_latestStatusReason = QString("CLEAR (Holding - RTT %1ms/Base %2ms)").arg(smoothedRtt, 0, 'f', 0).arg(m_rttMin, 0, 'f', 0);
                return;
            }

            // If physical link bandwidth was previously restricted to extreme survival levels (<= 400 kbps)
            // and user opens bandwidth limit to broadband (>= 1000 kbps), lift the ceiling to match.
            // (Does NOT touch normal operational ceilings like 1800k, 2100k, etc.)
            if (m_lastCongestedBitrate > 0 && m_lastCongestedBitrate <= 400u && bandwidthCapKbps >= 1000u &&
                m_lossPercentAvg == 0.0 && rttInflation < 4.0) {
                unsigned int oldCeiling = m_lastCongestedBitrate;
                m_lastCongestedBitrate = bandwidthCapKbps;
                m_lastProbeTime = ctime;
                qInfo().noquote() << QString("[QoS Engine] Network capacity expanded: Ceiling lifted from %1 kbps to %2 kbps")
                                         .arg(oldCeiling).arg(m_lastCongestedBitrate);
            }

            // In a given network environment, once a failure ceiling is recorded,
            // strictly LOCK it.
            unsigned int effectiveMax = targetProfileBitrate;
            if (m_lastCongestedBitrate > 0) {
                effectiveMax = qMin(effectiveMax, m_lastCongestedBitrate);
            }
            if (m_hasSiyiTelemetry && m_siyiStat.isAlive && m_currentEnvCeiling > 0) {
                effectiveMax = qMin(effectiveMax, m_currentEnvCeiling);
            }
            effectiveMax = (effectiveMax / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            effectiveMax = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, effectiveMax, m_maxBitrateKbps);

            if (m_currentBitrateKbps >= effectiveMax) {
                // ĐÃ CHẠM MỨC TRẦN CỦA MÔI TRƯỜNG HIỆN TẠI:
                // KHÓA CỨNG HOÀN TOÀN! THÀNH 1 ĐƯỜNG THẲNG TẮP!
                // TUYỆT ĐỐI KHÔNG BÒ LÊN THĂM DÒ NỮA KHI MÔI TRƯỜNG MẠNG CHƯA ĐỔI!
                m_latestStatusReason = QString("CLEAR (LOCKED at %1 | %2k)")
                                           .arg(m_hasSiyiTelemetry ? m_currentEnvName : "Ceiling")
                                           .arg(effectiveMax);
                return;
            } else {
                m_lastProbeTime = 0;

                // Strictly require ZERO loss before incrementing:
                // If there is ANY loss (even 0.5%), HOLD current bitrate!
                if (m_lossPercentAvg > 0.0) {
                    m_latestStatusReason = QString("CLEAR (Holding - Loss %1%)").arg(m_lossPercentAvg, 0, 'f', 1);
                    return;
                }

                // Smooth and gradual ramp-up (fast recovery when far below capacity, gentle near ceiling)
                unsigned int headroom = (effectiveMax > m_currentBitrateKbps) ? (effectiveMax - m_currentBitrateKbps) : 0;
                unsigned int stepKbps = BITRATE_ROUNDING_STEP_KBPS;
                if (headroom >= 500u) {
                    stepKbps = 200u;
                } else if (headroom >= 250u) {
                    stepKbps = 100u;
                }
                unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                targetBitrate = qMin(targetBitrate, effectiveMax);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                if (targetBitrate > m_currentBitrateKbps) {
                    m_previousStableBitrate = m_currentBitrateKbps;
                    m_lastBitrateIncrTime = ctime;
                    m_lastBitrateChangeTime = ctime;
                    applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
                }
            }
        }
    }
    else {
        if (m_lastCongestionState == CongestionState::Panic && state != CongestionState::Panic) {
            if (ctime - m_lastKeyframeRequestTime >= 3000) {
                m_lastKeyframeRequestTime = ctime;
                emit requestKeyframe();
            }
        }

        // Track onset of congestion for transient occlusion debounce:
        if (m_congestionStartTimeMs == 0) {
            m_congestionStartTimeMs = ctime;
        }
        m_consecutiveCongestedSamples++;

        // Transient RF occlusion filter:
        // When a drone is temporarily occluded (< 1s, samples < 2) and loss is not Panic,
        // HOLD current bitrate to give SRT buffer and RF link time to recover without dropping video quality.
        const bool isPanicEmergency = (state == CongestionState::Panic || m_lossPercentAvg >= LOSS_THRESHOLD_PANIC_PERCENT);
        const bool isDebounceHolding = (!m_wasCongested && !isPanicEmergency &&
                                        (ctime - m_congestionStartTimeMs < TRANSIENT_OCCLUSION_DEBOUNCE_MS ||
                                         m_consecutiveCongestedSamples < 2));

        if (isDebounceHolding) {
            double elapsedSec = (ctime - m_congestionStartTimeMs) / 1000.0;
            m_latestStatusReason = QString("HOLD (Occlusion debounce %1s)").arg(elapsedSec, 0, 'f', 1);
            return;
        }

        if (!m_wasCongested) {
            m_wasCongested = true;
            m_clearSinceMs = 0;

            // Determine previous safe bitrate before congestion occurred:
            // This is the bitrate immediately before congestion occurred (e.g. if failed at 950, safe was 900).
            unsigned int previousSafe = (m_previousStableBitrate > 0 && m_previousStableBitrate < m_currentBitrateKbps)
                ? m_previousStableBitrate
                : ((m_currentBitrateKbps > 50) ? (m_currentBitrateKbps - 50) : MIN_ACTIVE_VIDEO_BITRATE_KBPS);

            // Dynamic safe ceiling:
            // Directly lock ceiling to the previous safe operating bitrate.
            unsigned int safeCeiling = previousSafe;
            safeCeiling = (safeCeiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            safeCeiling = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, safeCeiling, m_maxBitrateKbps);

            // Lock ceiling:
            // The ceiling must NEVER exceed the safe rate of the bitrate that just congested!
            unsigned int prevCeiling = m_lastCongestedBitrate;
            if (m_lastCongestedBitrate == 0) {
                m_lastCongestedBitrate = safeCeiling;
            } else {
                m_lastCongestedBitrate = qMin(m_lastCongestedBitrate, safeCeiling);
            }
            m_lastCongestedBitrate = (m_lastCongestedBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            m_lastCongestedBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, m_lastCongestedBitrate, m_maxBitrateKbps);

            m_lastProbeTime = ctime;
            if (m_lastCongestedBitrate != prevCeiling) {
                qInfo().noquote() << QString("[QoS Engine] Bitrate ceiling: %1 kbps").arg(m_lastCongestedBitrate);
            }

            // Cut bitrate back smoothly:
            unsigned int immediateTarget = safeCeiling;
            if (state == CongestionState::Panic || m_lossPercentAvg >= LOSS_THRESHOLD_PANIC_PERCENT) {
                m_consecutivePanicCount = 1;
                // Graceful backoff: drop 35% - 40% (minimum 300 kbps) to clear queues immediately without collapsing video
                unsigned int backoffTarget = static_cast<unsigned int>(m_currentBitrateKbps * 0.65);
                backoffTarget = qMax(300u, backoffTarget);
                immediateTarget = qMin(backoffTarget, safeCeiling);
                immediateTarget = qMin(immediateTarget, bandwidthCapKbps);
                m_cooldownUntilMs = ctime + 1500;
            } else if (state == CongestionState::Severe || m_lossPercentAvg >= LOSS_THRESHOLD_SEVERE_PERCENT) {
                m_consecutivePanicCount = 0;
                unsigned int stepDown = qMax(100u, static_cast<unsigned int>(m_currentBitrateKbps * 0.20));
                immediateTarget = (safeCeiling > stepDown) ? (safeCeiling - stepDown) : MIN_ACTIVE_VIDEO_BITRATE_KBPS;
                immediateTarget = qMin(immediateTarget, bandwidthCapKbps);
                m_cooldownUntilMs = ctime + 1500;
            } else {
                // Moderate bottleneck ceiling hit (loss 2.5% - 8%): immediate fallback to safeCeiling
                m_consecutivePanicCount = 0;
                immediateTarget = qMin(safeCeiling, bandwidthCapKbps);
                m_cooldownUntilMs = ctime + 1000;
            }

            immediateTarget = (immediateTarget / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            immediateTarget = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, immediateTarget, m_maxBitrateKbps);

            if (immediateTarget < m_currentBitrateKbps) {
                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(immediateTarget, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
        else {
            // Already in congestion state: handle further step-downs only after cooldown has expired and buffers had time to drain
            bool cooldownExpired = (ctime >= m_cooldownUntilMs);
            if (cooldownExpired && timeSinceLastChange >= requiredInterval) {
                unsigned int targetBitrate = m_currentBitrateKbps;

                if (state == CongestionState::Panic || m_lossPercentAvg >= LOSS_THRESHOLD_PANIC_PERCENT) {
                    m_consecutivePanicCount++;
                    if (m_consecutivePanicCount == 2 && m_currentBitrateKbps > 400) {
                        targetBitrate = qMax(250u, static_cast<unsigned int>(m_currentBitrateKbps * 0.50));
                        targetBitrate = qMin(targetBitrate, bandwidthCapKbps);
                    } else if (m_consecutivePanicCount <= 3 && m_currentBitrateKbps > 200) {
                        targetBitrate = qMax(150u, static_cast<unsigned int>(m_currentBitrateKbps * 0.40));
                        targetBitrate = qMin(targetBitrate, bandwidthCapKbps);
                    } else {
                        // Sustained Panic (>= 4 samples): Total link collapse
                        targetBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
                    }
                    m_cooldownUntilMs = ctime + ((targetBitrate > MIN_ACTIVE_VIDEO_BITRATE_KBPS) ? 1000 : RECOVERY_COOLDOWN_MS);
                } else if (state == CongestionState::Severe || m_lossPercentAvg >= LOSS_THRESHOLD_SEVERE_PERCENT) {
                    unsigned int stepDown = qMax(150u, static_cast<unsigned int>(m_currentBitrateKbps * 0.25));
                    targetBitrate = (m_currentBitrateKbps > stepDown) ? (m_currentBitrateKbps - stepDown) : MIN_ACTIVE_VIDEO_BITRATE_KBPS;
                    targetBitrate = qMin(targetBitrate, bandwidthCapKbps);
                    m_cooldownUntilMs = ctime + 1500;
                } else {
                    unsigned int stepDown = (m_currentBitrateKbps <= 600) ? 50u : 100u;
                    targetBitrate = (m_currentBitrateKbps > stepDown) ? (m_currentBitrateKbps - stepDown) : MIN_ACTIVE_VIDEO_BITRATE_KBPS;
                    targetBitrate = qMin(targetBitrate, bandwidthCapKbps);
                    m_cooldownUntilMs = ctime + 1000;
                }

                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                if (targetBitrate < m_currentBitrateKbps) {
                    m_lastBitrateChangeTime = ctime;
                    applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
                }
            }
        }
    }
}

void SRTAdaptiveBitrateStreaming::applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss)
{
    Q_UNUSED(rtt);
    Q_UNUSED(bandwidthMbps);
    Q_UNUSED(deltaLoss);

    if (m_isExplicitC2Only) {
        const unsigned int c2Cap = m_isStrictVideoCutoff
                                       ? VIDEO_DISABLED_BITRATE_KBPS
                                       : MIN_ACTIVE_VIDEO_BITRATE_KBPS;
        if (targetBitrateKbps > c2Cap) {
            targetBitrateKbps = c2Cap;
        }
    }

    if (targetBitrateKbps == 0) {
        if (m_currentBitrateKbps != 0) {
            m_currentBitrateKbps = 0;
            emit bitrateChanged(m_currentBitrateKbps);
        }
        return;
    }

    targetBitrateKbps = (targetBitrateKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
    targetBitrateKbps = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrateKbps, m_maxBitrateKbps);

    if (targetBitrateKbps == m_currentBitrateKbps) {
        return;
    }

    m_currentBitrateKbps = targetBitrateKbps;
    emit bitrateChanged(m_currentBitrateKbps);
}
