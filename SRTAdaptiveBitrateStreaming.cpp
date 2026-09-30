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
    , m_lastCameraQosTime(0)
    , m_lastActiveTrafficTime(0)
    , m_isTrafficIdle(true)
    , m_isVideoCollapsed(false)
    , m_needRecoveryRefresh(false)
    , m_lastCollapseLogTime(0)
    , m_lastAchievedBitrateKbps(0)
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
    m_lastCongestedBitrate = 0;
    m_lastProbeTime = 0;
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
    qInfo().noquote() << QString("[%1] RTT: %2ms | BW: %3 Mbps | Loss: %4% | Video: %5 | State: %6 --> bitrate: %7")
        .arg(timeStr)
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestSmoothedLossPercent, 0, 'f', 1)
        .arg(videoStateLabel)
        .arg(m_latestStatusReason)
        .arg(videoStr);
}

void SRTAdaptiveBitrateStreaming::handleCameraReportedBitrate(int achievedKbps)
{
    if (!m_isRunning) return;
    m_lastAchievedBitrateKbps = (achievedKbps > 0) ? static_cast<unsigned int>(achievedKbps) : 0;
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

SRTAdaptiveBitrateStreaming::CongestionState SRTAdaptiveBitrateStreaming::classifyCongestion(
    double lossPercent, double rtt, double rttInflation, bool useExitThresholds, bool hasLatencyDrops, bool isKeyframeBurst) const
{
    const double f = useExitThresholds ? 0.85 : 1.0;
    const double rttScale   = useExitThresholds ? 0.88 : 1.0;
    const double inflScale  = useExitThresholds ? 0.85 : 1.0;

    const double relInflation = (m_rttMin > 0.0) ? (rttInflation / m_rttMin) : 0.0;
    const bool isLowLatencyLink = (rtt <= 25.0 && rttInflation < 10.0 && !hasLatencyDrops);
    const double panicLossThreshold = isLowLatencyLink ? (28.0 * f) : (18.0 * f);

    // Khi vừa gửi keyframe, I-frame kích thước lớn làm hàng đợi phình to tạm thời trong 800ms.
    // Nếu không có mất gói cao (lossPercent < 10%), tạm thời không kích hoạt Panic ảo do RTT spike.
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

    const bool hasActiveTraffic = (deltaSent > 0 || rawSendRateMbps >= 0.05);
    if (hasActiveTraffic) {
        m_lastActiveTrafficTime = ctime;
        if (m_isTrafficIdle) {
            m_isTrafficIdle = false;
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
            // Đặt sàn mẫu tối thiểu 20 gói để tránh nhiễu mẫu nhỏ ở bitrate thấp (400 kbps)
            // (Ví dụ chỉ rớt 2 gói trong 8 gói không bị nhân lên thành 25% gây false Panic)
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
            m_lossPercentAvg = (m_lossPercentAvg * 0.35) + (instantLossPercent * 0.65);
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

    unsigned int bandwidthCapKbps = m_maxBitrateKbps;

    // 1. Áp dụng trần từ probe SRT hợp lệ (nếu đo được và <= 150 Mbps)
    if (smoothedBw > 0.1 && smoothedBw <= 150.0) {
        double capKbps = smoothedBw * 1000.0 * BW_UTILIZATION_RATIO;
        if (capKbps >= MIN_ACTIVE_VIDEO_BITRATE_KBPS && capKbps < static_cast<double>(bandwidthCapKbps)) {
            bandwidthCapKbps = static_cast<unsigned int>(capKbps);
        }
    }

    if (m_lossPercentAvg >= 2.0) {
        double currentRateMbps = (rawSendRateMbps > 0.2) 
                                     ? rawSendRateMbps 
                                     : (static_cast<double>(m_currentBitrateKbps) / 1000.0);
        double lossFraction = qBound(0.0, m_lossPercentAvg / 100.0, 0.95);
        double goodputMbps = currentRateMbps * (1.0 - lossFraction);
        double safeCapKbps = goodputMbps * 1000.0 * 0.85;

        unsigned int estimatedCap = static_cast<unsigned int>(qMax(static_cast<double>(MIN_ACTIVE_VIDEO_BITRATE_KBPS), safeCapKbps));
        bandwidthCapKbps = qMin(bandwidthCapKbps, estimatedCap);

        if (smoothedBw > goodputMbps || smoothedBw <= 0.0) {
            smoothedBw = goodputMbps;
            m_latestSmoothedBw = smoothedBw;
        }
    }

    // Live Bootstrapping: initial bitrate from first partition state
    if (!m_isBootstrapped) {
        unsigned int calculatedInitialBitrate = m_maxBitrateKbps;

        switch (state) {
        case CongestionState::Panic:
            calculatedInitialBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::Severe:
            calculatedInitialBitrate = 1000;
            m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
            break;
        case CongestionState::Moderate:
            calculatedInitialBitrate = 2200;
            m_cooldownUntilMs = ctime + 1000;
            break;
        case CongestionState::Clear:
        default:
            calculatedInitialBitrate = m_maxBitrateKbps;
            break;
        }

        calculatedInitialBitrate = qMin(calculatedInitialBitrate, bandwidthCapKbps);
        calculatedInitialBitrate = (calculatedInitialBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;

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
        m_latestStatusReason = "\033[1;31mPANIC (360p 250k)\033[0m";
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
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
        bool clearStableMet = (ctime - m_clearSinceMs >= CLEAR_STABLE_DURATION_MS);

        if (cooldownExpired && decisionIntervalExpired && clearStableMet) {
            if (!isBufferDrained) {
                m_latestStatusReason = QString("CLEAR (Holding - RTT %1ms/Base %2ms)").arg(smoothedRtt, 0, 'f', 0).arg(m_rttMin, 0, 'f', 0);
                return;
            }

            // Probing thận trọng vượt trần nghẽn cũ: chỉ khi đã chạy ổn định ở trần ít nhất FAILURE_MEMORY_PROBE_INTERVAL_MS
            if (m_lastCongestedBitrate > 0 && m_currentBitrateKbps >= m_lastCongestedBitrate) {
                if (m_lastProbeTime == 0) {
                    m_lastProbeTime = ctime;
                }
                if (ctime - m_lastProbeTime >= FAILURE_MEMORY_PROBE_INTERVAL_MS && m_lossPercentAvg == 0.0) {
                    m_lastProbeTime = ctime;
                    m_lastCongestedBitrate = qMin(m_lastCongestedBitrate + FAILURE_MEMORY_PROBE_STEP_KBPS,
                                                  m_maxBitrateKbps);
                }
            }

            unsigned int effectiveMax = qMin(targetProfileBitrate, bandwidthCapKbps);
            if (m_lastCongestedBitrate > 0) {
                effectiveMax = qMin(effectiveMax, m_lastCongestedBitrate);
            }

            if (m_currentBitrateKbps < effectiveMax) {
                unsigned int stepKbps;
                if (m_lastCongestedBitrate > 0 && m_currentBitrateKbps >= static_cast<unsigned int>(m_lastCongestedBitrate * 0.70)) {
                    // Cận kề trần tắc nghẽn: tăng từng bước nhỏ 100 kbps để không vọt quá năng lực đường truyền
                    stepKbps = 100;
                } else if (m_currentBitrateKbps < 800) {
                    stepKbps = 200; 
                } else if (m_currentBitrateKbps < 2000) {
                    stepKbps = 300; 
                } else {
                    stepKbps = 400; 
                }

                unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                targetBitrate = qMin(targetBitrate, effectiveMax);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);
                if (targetBitrate <= m_currentBitrateKbps && m_currentBitrateKbps < effectiveMax) {
                    targetBitrate = qMin(m_currentBitrateKbps + BITRATE_ROUNDING_STEP_KBPS, effectiveMax);
                }

                if (targetBitrate > m_currentBitrateKbps) {
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
        m_wasCongested = true;

        if ((m_lossPercentAvg >= 18.0 || state == CongestionState::Panic) && (ctime - m_lastKeyframeRequestTime >= 4000)) {
            m_lastKeyframeRequestTime = ctime;
            emit requestKeyframe();
        }

        if (m_currentBitrateKbps > targetProfileBitrate) {
            m_clearSinceMs = 0;
            if (timeSinceLastChange >= requiredInterval) {
                unsigned int targetBitrate = targetProfileBitrate;

                if (state == CongestionState::Panic || m_lossPercentAvg >= 20.0) {
                    targetBitrate = qMin(targetProfileBitrate, bandwidthCapKbps);
                    targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                    targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                    if (m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS) {
                        unsigned int safeCeiling = static_cast<unsigned int>(m_currentBitrateKbps * 0.85);
                        safeCeiling = (safeCeiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                        m_lastCongestedBitrate = (m_lastCongestedBitrate > 0)
                            ? qMin(m_lastCongestedBitrate, safeCeiling)
                            : safeCeiling;
                        m_lastCongestedBitrate = qMax(MIN_ACTIVE_VIDEO_BITRATE_KBPS, m_lastCongestedBitrate);
                    }
                    m_lastProbeTime = ctime;
                    m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
                    m_lastBitrateChangeTime = ctime;
                    applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
                    return;
                }

                unsigned int stepDown;
                if (state == CongestionState::Severe || m_lossPercentAvg >= 8.0) {
                    stepDown = qMax(1200u, static_cast<unsigned int>(m_currentBitrateKbps * 0.35));
                } else {
                    stepDown = qMax(600u, static_cast<unsigned int>(m_currentBitrateKbps * 0.20));
                }

                if (m_currentBitrateKbps > stepDown && (m_currentBitrateKbps - stepDown) > targetProfileBitrate) {
                    targetBitrate = m_currentBitrateKbps - stepDown;
                }

                targetBitrate = qMin(targetBitrate, bandwidthCapKbps);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                if (m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS) {
                    unsigned int safeCeiling = static_cast<unsigned int>(m_currentBitrateKbps * 0.88);
                    safeCeiling = (safeCeiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                    m_lastCongestedBitrate = (m_lastCongestedBitrate > 0)
                        ? qMin(m_lastCongestedBitrate, safeCeiling)
                        : safeCeiling;
                    m_lastCongestedBitrate = qMax(MIN_ACTIVE_VIDEO_BITRATE_KBPS, m_lastCongestedBitrate);
                }
                m_lastProbeTime = ctime;

                m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
        else if (m_currentBitrateKbps < targetProfileBitrate) {
            m_clearSinceMs = 0;

            // Never ramp up bitrate while remaining in Panic state
            if (state == CongestionState::Panic) {
                return;
            }

            bool cooldownExpired = (ctime >= m_cooldownUntilMs);
            bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
            bool lossSafeForRamp = (m_lossPercentAvg < 2.0) || (m_consecutiveZeroLossCount >= 2);

            if (cooldownExpired && decisionIntervalExpired && lossSafeForRamp) {
                unsigned int rampLimit = qMin(targetProfileBitrate,
                                              qMax(bandwidthCapKbps, MIN_ACTIVE_VIDEO_BITRATE_KBPS));
                if (m_lastCongestedBitrate > 0) {
                    rampLimit = qMin(rampLimit, m_lastCongestedBitrate);
                }

                unsigned int stepUp = (state == CongestionState::Severe) ? 200u : 300u;
                unsigned int targetBitrate = m_currentBitrateKbps + stepUp;
                targetBitrate = qMin(rampLimit, targetBitrate);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                if (targetBitrate > m_currentBitrateKbps) {
                    m_lastBitrateIncrTime = ctime;
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
