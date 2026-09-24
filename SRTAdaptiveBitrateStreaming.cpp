#include "SRTAdaptiveBitrateStreaming.h"
#ifndef XBFIRM
#include "ABRConfigs.h"
#endif
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
#ifdef XBFIRM
    , m_isStrictVideoCutoff(false)
#else
    , m_isStrictVideoCutoff(ABR_DEFAULT_C2_STRICT_VIDEO_CUTOFF)
#endif
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
    , m_isVideoCollapsed(false)
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
    m_isVideoCollapsed = false;
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
    m_isVideoCollapsed = false;
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

void SRTAdaptiveBitrateStreaming::setC2StrictVideoCutoff(bool strict)
{
    if (m_isStrictVideoCutoff == strict) {
        return;
    }
    m_isStrictVideoCutoff = strict;
    qInfo() << "[BelaCoder-SRT] C2 video policy:"
            << (strict ? "STRICT_CUTOFF (0 kbps)" : "SURVIVAL_FLOOR (400 kbps)");
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

void SRTAdaptiveBitrateStreaming::handleQosControllingConnection(const QVector<SRTPeerStat> &peers)
{

    if (!m_isRunning || peers.isEmpty()) return;

    bool explicitC2Only = false;
    for (const SRTPeerStat &p : peers) {
        explicitC2Only = explicitC2Only || p.isC2Only;
    }
    if (explicitC2Only != m_isExplicitC2Only) {
        m_isExplicitC2Only = explicitC2Only;
        evaluateC2Quality();
    }
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
                qWarning().noquote() << QString(">>> [QoS WATCHDOG] ⚠️ CAMERA OUTPUT COLLAPSED: "
                                                "commanded %1 kbps but output only %2 kbps <<<")
                                            .arg(m_currentBitrateKbps)
                                            .arg(m_lastAchievedBitrateKbps);
            }
        } else if (m_isVideoCollapsed) {
            m_isVideoCollapsed = false;
            qInfo().noquote() << QString(">>> [QoS WATCHDOG] ✅ Camera output recovered: %1 kbps (commanded %2 kbps) <<<")
                                        .arg(m_lastAchievedBitrateKbps)
                                        .arg(m_currentBitrateKbps);
        }
    }

    QString videoStr;
    if (!m_isVideoEnabled) {
        videoStr = "OFF";
    } else if (m_isVideoCollapsed || m_lastCongestionState == CongestionState::Panic) {
        videoStr = QString("\033[1;31m%1 kbps (COLLAPSED)\033[0m").arg(m_currentBitrateKbps);
    } else {
        videoStr = QString("\033[1;32m%1 kbps\033[0m").arg(m_currentBitrateKbps);
    }

    QString videoStateLabel;
    if (!m_isVideoEnabled) {
        videoStateLabel = "OFF";
    } else if (m_isVideoCollapsed || m_lastCongestionState == CongestionState::Panic) {
        videoStateLabel = "\033[1;31mCOLLAPSED\033[0m";
    } else {
        videoStateLabel = "ON";
    }

    QString achievedStr = (m_lastAchievedBitrateKbps > 0)
                              ? QString("%1 kbps").arg(m_lastAchievedBitrateKbps)
                              : QString("n/a");

    qInfo().noquote() << QString("[QoS] RTT: %1ms | BW: %2 Mbps | Loss: %3 (%4%) | Cmd: %5 | Out: %6 | Video: %7 | State: %8")
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestDeltaLoss)
        .arg(m_latestSmoothedLossPercent, 0, 'f', 1)
        .arg(videoStr)
        .arg(achievedStr)
        .arg(videoStateLabel)
        .arg(m_latestStatusReason);
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

SRTAdaptiveBitrateStreaming::CongestionState SRTAdaptiveBitrateStreaming::classifyCongestion(double lossPercent, double rtt, double rttInflation, bool useExitThresholds, bool hasLatencyDrops) const
{
    const double f = useExitThresholds ? 0.90 : 1.0;
    const double rttScale   = useExitThresholds ? 0.92 : 1.0;
    const double inflScale  = useExitThresholds ? 0.90 : 1.0;

    if (lossPercent >= 45.0 * f || (hasLatencyDrops && lossPercent >= 30.0 * f) || (rtt >= 300.0 * rttScale && rttInflation > 200.0 * inflScale)) {
        return CongestionState::Panic;
    }
    if (lossPercent >= 32.0 * f || (rtt >= 220.0 * rttScale && rttInflation > 140.0 * inflScale)) {
        return CongestionState::Extreme;
    }
    if (lossPercent >= 20.0 * f || (rtt >= 150.0 * rttScale && rttInflation > 90.0 * inflScale)) {
        return CongestionState::HeavySevere;
    }
    if (lossPercent >= 10.0 * f || (rtt >= 100.0 * rttScale && rttInflation > 60.0 * inflScale)) {
        return CongestionState::HeavyModerate;
    }
    if (lossPercent >= 4.0 * f || (rtt >= 75.0 * rttScale && rttInflation > 45.0 * inflScale)) {
        return CongestionState::HeavyLight;
    }
    if (lossPercent >= 1.5 * f || (rtt >= 55.0 * rttScale && rttInflation > 30.0 * inflScale)) {
        return CongestionState::Light;
    }
    return CongestionState::Clear;
}

void SRTAdaptiveBitrateStreaming::processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps,
                                                int rawLossTotal, qint64 rawSentTotal,
                                                int rawDropSndTotal, int rawDropRcvTotal,
                                                int rawRetransTotal)
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

    double dt = (m_lastQosPacketTime > 0) ? (ctime - m_lastQosPacketTime) / 1000.0 : 0.25;
    bool isStaleSample = (dt > 2.0);
    if (isStaleSample) {
        qWarning() << "[BelaCoder-SRT] QoS gap" << dt << "s - holding current bitrate";
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
            qInfo() << "[BelaCoder-SRT] RTT baseline seeded:" << m_rttMin << "ms (from"
                    << m_rttWarmupHistory.size() << "samples)";
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

    if (lossReset) {
        qWarning() << "[BelaCoder-SRT] Loss counter reset detected ("
                   << m_lastLossTotal << "->" << rawLossTotal << "), skipping sample";
    }
    if (dropSndReset || dropRcvReset) {
        qWarning() << "[BelaCoder-SRT] Drop counter reset detected (snd"
                   << m_lastDropSndTotal << "->" << rawDropSndTotal << ", rcv"
                   << m_lastDropRcvTotal << "->" << rawDropRcvTotal << ")";
    }

    m_lastLossTotal = rawLossTotal;
    m_hasLastLoss = true;
    m_lastDropSndTotal = rawDropSndTotal;
    m_hasLastDropSnd = true;
    m_lastDropRcvTotal = rawDropRcvTotal;
    m_hasLastDropRcv = true;
    m_lastRetransTotal = rawRetransTotal;
    m_hasLastRetrans = true;

    const int deltaLoss = static_cast<int>(deltaLossRaw);

    const qint64 wireLostPkts = qMax(deltaLossRaw, deltaRetrans);
    const qint64 deltaFailures = wireLostPkts + deltaDropSnd + deltaDropRcv;

    double instantLossPercent = 0.0;
    if (deltaFailures > 0) {
        if (deltaSent > 0) {

            instantLossPercent = (static_cast<double>(deltaFailures) / static_cast<double>(deltaSent)) * 100.0;
        } else {
            double currentRateMbps = (rawSendRateMbps > 0.1) ? rawSendRateMbps : (static_cast<double>(m_currentBitrateKbps) / 1000.0);
            double expectedPkts = (currentRateMbps * 1000000.0 * dt) / (1316.0 * 8.0);
            if (expectedPkts < 5.0) {
                expectedPkts = 5.0;
            }
            instantLossPercent = (static_cast<double>(deltaFailures) / expectedPkts) * 100.0;
        }
    }
    instantLossPercent = qBound(0.0, instantLossPercent, 100.0);

    if (deltaFailures == 0) {
        m_consecutiveZeroLossCount++;
        if (m_consecutiveZeroLossCount >= 4) {
            m_lossPercentAvg *= 0.60;
            if (m_lossPercentAvg < 1.0) {
                m_lossPercentAvg = 0.0;
            }
        } else {
            m_lossPercentAvg *= 0.88;
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

    const int currentSeverity = static_cast<int>(m_lastCongestionState);
    const int enterSeverity = static_cast<int>(
        classifyCongestion(m_lossPercentAvg, smoothedRtt, rttInflation, false, hasLatencyDrops));
    const int exitSeverity = static_cast<int>(
        classifyCongestion(m_lossPercentAvg, smoothedRtt, rttInflation, true, hasLatencyDrops));

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
            qWarning().noquote() << QString(">>> [STREAM COLLAPSE] ⚠️ LATENCY CEILING BROKEN (SRT Loss: %1%, Failures: %2, Drops: %3) | Stream has collapsed, forcing survival floor %4 kbps! <<<")
                                        .arg(m_lossPercentAvg, 0, 'f', 1)
                                        .arg(deltaFailures)
                                        .arg(deltaDropSnd + deltaDropRcv)
                                        .arg(MIN_ACTIVE_VIDEO_BITRATE_KBPS);
        }
    }

    if (isStaleSample) {
        return;
    }

    unsigned int bandwidthCapKbps = m_maxBitrateKbps;
    if (smoothedBw > 0.0 && m_lossPercentAvg < 15.0) {
        double capKbps = smoothedBw * 1000.0 * BW_UTILIZATION_RATIO;
        if (capKbps < static_cast<double>(bandwidthCapKbps)) {
            bandwidthCapKbps = static_cast<unsigned int>(capKbps);
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

        calculatedInitialBitrate = qMin(calculatedInitialBitrate, bandwidthCapKbps);
        calculatedInitialBitrate = (calculatedInitialBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;

        m_isBootstrapped = true;
        m_lastBitrateChangeTime = ctime;
        m_lastBitrateIncrTime = ctime;

        applyNewBitrate(calculatedInitialBitrate, smoothedRtt, smoothedBw, deltaLoss);
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
        m_latestStatusReason = QString("\033[1;31mSTREAM_COLLAPSE (Loss %1% - 360p 400k)\033[0m").arg(m_lossPercentAvg, 0, 'f', 1);
    }

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
        if (m_clearSinceMs == 0) {
            m_clearSinceMs = ctime;
        }

        if (m_wasCongested) {
            m_wasCongested = false;
            if (ctime - m_lastKeyframeRequestTime >= KEYFRAME_REQUEST_COOLDOWN_MS) {
                m_lastKeyframeRequestTime = ctime;
                qInfo().noquote() << QString("[QoS Recovery] Exited congestion (RTT %1ms/base %2ms). "
                                             "Requesting new Keyframe to flush decoder (cooldown %3s)")
                                          .arg(smoothedRtt, 0, 'f', 0)
                                          .arg(m_rttMin, 0, 'f', 0)
                                          .arg(KEYFRAME_REQUEST_COOLDOWN_MS / 1000);
                emit requestKeyframe();
            } else {
                qDebug().noquote() << QString("[QoS Recovery] Keyframe request blocked by cooldown (%1s remaining)")
                                          .arg((KEYFRAME_REQUEST_COOLDOWN_MS - (ctime - m_lastKeyframeRequestTime)) / 1000.0, 0, 'f', 1);
            }
        }

        bool isBufferDrained = (rttInflation < 30.0 || smoothedRtt <= m_rttMin * 1.30 + 15.0);

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);
        bool clearStableMet = (ctime - m_clearSinceMs >= CLEAR_STABLE_DURATION_MS);

        if (cooldownExpired && decisionIntervalExpired && clearStableMet) {
            if (m_lastCongestedBitrate > 0 && m_currentBitrateKbps >= m_lastCongestedBitrate) {
                m_lastCongestedBitrate = qMin(m_lastCongestedBitrate + FAILURE_MEMORY_PROBE_STEP_KBPS,
                                              m_maxBitrateKbps);
            }

            unsigned int effectiveMax = qMin(targetProfileBitrate, bandwidthCapKbps);
            if (m_lastCongestedBitrate > 0) {
                effectiveMax = qMin(effectiveMax, m_lastCongestedBitrate);
            }

            if (m_currentBitrateKbps < effectiveMax) {
                unsigned int stepKbps;
                if (!isBufferDrained) {
                    m_latestStatusReason = QString("CLEAR (Xa buffer - RTT %1ms/Base %2ms)").arg(smoothedRtt, 0, 'f', 0).arg(m_rttMin, 0, 'f', 0);
                    stepKbps = 150;
                } else {
                    stepKbps = (m_currentBitrateKbps < 2000) ? 800 : 1000;
                }

                unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                targetBitrate = qMin(targetBitrate, effectiveMax);
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastBitrateIncrTime = ctime;
                m_lastBitrateChangeTime = ctime;

                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
    }
    else {
        m_wasCongested = true;

        if (m_currentBitrateKbps > targetProfileBitrate) {
            m_clearSinceMs = 0;
            if (timeSinceLastChange >= requiredInterval) {
                unsigned int targetBitrate = targetProfileBitrate;
                targetBitrate = (targetBitrate / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                targetBitrate = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, targetBitrate, m_maxBitrateKbps);

                m_lastCongestedBitrate = qMax(m_lastCongestedBitrate, m_currentBitrateKbps);

                m_cooldownUntilMs = ctime + RECOVERY_COOLDOWN_MS;
                m_lastBitrateChangeTime = ctime;
                applyNewBitrate(targetBitrate, smoothedRtt, smoothedBw, deltaLoss);
            }
        }
        else if (m_currentBitrateKbps < targetProfileBitrate) {
            m_clearSinceMs = 0;
            bool cooldownExpired = (ctime >= m_cooldownUntilMs);
            bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= BITRATE_INCR_DECISION_INTERVAL_MS);

            if (cooldownExpired && decisionIntervalExpired) {
                unsigned int rampLimit = qMin(targetProfileBitrate,
                                              qMax(bandwidthCapKbps, MIN_ACTIVE_VIDEO_BITRATE_KBPS));

                unsigned int stepUp = 400;
                unsigned int targetBitrate = m_currentBitrateKbps + stepUp;
                targetBitrate = qMin(rampLimit, targetBitrate);
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
