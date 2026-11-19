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
    , m_probeHealthySince(0)
    , m_probeStartedAt(0)
    , m_probeRetryDelayMs(FAILURE_MEMORY_PROBE_INTERVAL_MS)
    , m_probePreviousCeiling(0)
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
    , m_rfRecoveryRampActive(false)
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
    , m_currentEnvCeiling(RF_ENVIRONMENTS[0].ceilingKbps)
    , m_currentEnvName(RF_ENVIRONMENTS[0].name)
    , m_hasSiyiTelemetry(false)
{
    m_c2RateTracker = C2BitrateGuard(m_maxBitrateKbps, SRTTuning::C2_SURVIVAL_FLOOR_KBPS);
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &SRTAdaptiveBitrateStreaming::onHeartbeatTimeout);

    qInfo() << "[QoS Engine] Initialized ABR service. Default Bitrate:" << m_currentBitrateKbps << "kbps";
}

void SRTAdaptiveBitrateStreaming::start()
{
    m_isRunning = true;
    m_c2RateClock.start();
    m_c2RateTracker = C2BitrateGuard(m_maxBitrateKbps, SRTTuning::C2_SURVIVAL_FLOOR_KBPS);
    m_currentBitrateKbps = 0;
    m_recovery = SRTRecoveryState();
    m_appliedIncreaseSince = 0;
    m_lastAchievedBitrateKbps = 0;
    m_stalledSinceMs = 0;
    m_risingLossSamples = 0;
    m_previousRecoverySampleHadLoss = false;
    m_previousRawRecoveryRtt = 0.0;
    m_cameraPeerSet.clear();
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_bandwidthCapKbps = m_maxBitrateKbps;
    m_rttWarmupHistory.clear();
    m_siyiRssiHistory.clear();

    m_smoothedRssi = -100.0;
    m_smoothedSignal = 0;
    m_lastSiyiPacketTime = 0;
    m_currentEnvCeiling = RF_ENVIRONMENTS[0].ceilingKbps;
    m_currentEnvName = RF_ENVIRONMENTS[0].name;
    m_hasSiyiTelemetry = false;
    m_rfRecoveryRampActive = false;
    m_rfUpgradeFirstStepPending = false;

    m_isVideoEnabled = true;
    m_isExplicitC2Only = false;
    m_clearSinceMs = 0;
    m_clearBandwidthOverCapSamples = 0;
    m_lastCongestedBitrate = 0;
    m_lastProbeTime = 0;
    m_probeHealthySince = 0;
    m_probeStartedAt = 0;
    m_probePreviousCeiling = 0;
    m_probeRetryDelayMs = FAILURE_MEMORY_PROBE_INTERVAL_MS;
    m_previousStableBitrate = 0;
    m_bottleneckBwMbps = 0.0;
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
    m_cameraQosRecoverySamplesRemaining = 0;
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
    m_c2RateClock.start();
    m_c2RateTracker = C2BitrateGuard(m_maxBitrateKbps, SRTTuning::C2_SURVIVAL_FLOOR_KBPS);
    unsigned int target = (bitrateKbps > 0) ? static_cast<unsigned int>(bitrateKbps) : DEFAULT_INITIAL_BITRATE_KBPS;
    m_currentBitrateKbps = qBound(m_minBitrateKbps, target, m_maxBitrateKbps);
    m_recovery = SRTRecoveryState();
    m_appliedIncreaseSince = 0;
    m_lastAchievedBitrateKbps = 0;
    m_stalledSinceMs = 0;
    m_risingLossSamples = 0;
    m_previousRecoverySampleHadLoss = false;
    m_previousRawRecoveryRtt = 0.0;
    m_cameraPeerSet.clear();
    m_rttHistory.clear();
    m_bwHistory.clear();
    m_bandwidthCapKbps = m_maxBitrateKbps;
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
    m_probeHealthySince = 0;
    m_probeStartedAt = 0;
    m_probePreviousCeiling = 0;
    m_probeRetryDelayMs = FAILURE_MEMORY_PROBE_INTERVAL_MS;
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
    m_clearBandwidthOverCapSamples = 0;
    m_rfRecoveryRampActive = false;
    m_rfUpgradeFirstStepPending = false;
    m_lastCameraQosTime = 0;
    m_lastActiveTrafficTime = 0;
    m_isTrafficIdle = true;
    m_isVideoCollapsed = false;
    m_needRecoveryRefresh = false;
    m_cameraQosRecoverySamplesRemaining = 0;
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
    m_currentEnvCeiling = RF_ENVIRONMENTS[0].ceilingKbps;
    m_currentEnvName = RF_ENVIRONMENTS[0].name;
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

void SRTAdaptiveBitrateStreaming::setSerialConnected(bool connected)
{
    m_serialConnected = connected;
    if (connected) return;
    m_c2RateTracker = C2BitrateGuard(m_maxBitrateKbps, SRTTuning::C2_SURVIVAL_FLOOR_KBPS);
    m_c2Rtt = 0.0;
    m_c2Retransmits = 0;
    m_c2Unacked = 0;
    m_isExplicitC2Only = false;
    m_lastC2PacketTime = 0;
    evaluateC2Quality();
}

void SRTAdaptiveBitrateStreaming::handleC2ConnectionStats(const QVector<SRTPeerStat> &peers)
{
    if (!m_isRunning || peers.isEmpty()) return;

    std::vector<C2BitrateGuard::Peer> samples;
    bool explicitC2Only = false;
    for (const SRTPeerStat &p : peers) {
        samples.push_back({(p.peerAddress + ':' + QString::number(p.peerPort)).toStdString(),
                           p.msRTT, p.pktRetransTotal, p.pktSndDropTotal, p.isC2Only});
        explicitC2Only = explicitC2Only || p.isC2Only;
    }
    m_c2RateTracker.receive(samples, m_c2RateClock.elapsed(), 0);
    m_c2Rtt = m_c2RateTracker.rtt;
    m_c2Retransmits = m_c2RateTracker.retransRate;
    m_c2Unacked = m_c2RateTracker.dropRate;
    m_isExplicitC2Only = explicitC2Only;
    m_lastC2PacketTime = QDateTime::currentMSecsSinceEpoch();

    if (m_hasSiyiTelemetry &&
        m_lastC2PacketTime - m_lastSiyiPacketTime <= SIYI_LINK_TIMEOUT_MS) {
        evaluateC2Quality();
    }
}


bool SRTAdaptiveBitrateStreaming::canResyncCameraBitrate() const
{
    return m_lastCameraQosTime > 0 &&
           QDateTime::currentMSecsSinceEpoch() - m_lastCameraQosTime < CAMERA_QOS_STALE_TIMEOUT_MS &&
           m_cameraQosRecoverySamplesRemaining == 0 && !m_isTrafficIdle;
}

void SRTAdaptiveBitrateStreaming::onHeartbeatTimeout()
{
    if (!m_isRunning) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_hasSiyiTelemetry || now - m_lastSiyiPacketTime > SIYI_LINK_TIMEOUT_MS) {
        handleSiyiLinkLost();
        // QoS visibility and the camera watchdog remain active without RF data.
    }
    bool hasRecentQos = (m_lastCameraQosTime > 0 &&
                         (now - m_lastCameraQosTime) < CAMERA_QOS_STALE_TIMEOUT_MS);

    if (!hasRecentQos) {
        if (m_lastCameraQosTime > 0) {
            const qint64 qosAgeMs = now - m_lastCameraQosTime;
            const bool useFallback = qosAgeMs >= CAMERA_QOS_FALLBACK_TIMEOUT_MS;
            cancelQosProbe(now, false);
            const bool reduceToFloor = useFallback && m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS;
            if (reduceToFloor || now - m_lastCollapseLogTime >= 5000) {
                m_lastCollapseLogTime = now;
                qWarning().noquote() << QString("[QoS WATCHDOG] Camera QoS stale for %1s (prefix 9990), %2")
                                            .arg(qosAgeMs / 1000.0, 0, 'f', 1)
                                            .arg(useFallback ? "using survival floor" : "holding bitrate target");
            }
            m_isVideoCollapsed = true;
            m_needRecoveryRefresh = true;
            if (reduceToFloor) {
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

    QString videoStateLabel = "ON";
    if (!m_isVideoEnabled) {
        videoStateLabel = "OFF";
    } else if (m_isTrafficIdle) {
        videoStateLabel = "IDLE";
    } else if (m_isVideoCollapsed || m_lastCongestionState == CongestionState::Panic) {
        videoStateLabel = "OFF";
    }

    if (!m_hasSiyiTelemetry)
        m_latestStatusReason = "RF UNAVAILABLE (QoS active; increases frozen)";

    const QString siyi = m_hasSiyiTelemetry
        ? QString("%1dBm").arg(m_smoothedRssi, 0, 'f', 0) : QStringLiteral("N/A");
    const bool hasC2 = m_c2RateTracker.hasFreshSample(m_c2RateClock.elapsed());
    const QString unavailable = QStringLiteral("N/A");
    const auto rate = [hasC2, &unavailable](double value) {
        if (!hasC2) return unavailable;
        return value == 0.0 ? QStringLiteral("0") : QString::number(value, 'f', 2);
    };
    QString state = m_latestStatusReason.section(" (", 0, 0);
    state.remove(QStringLiteral("\033[1;31m"));

    const QString c2Metrics = m_serialConnected
        ? QStringLiteral("C2_RTT: %1, C2_retrans/s: %2, C2_drop/s: %3, ")
            .arg(hasC2 ? QString("%1ms").arg(m_c2Rtt, 0, 'f', 2) : unavailable)
            .arg(rate(m_c2Retransmits))
            .arg(rate(m_c2Unacked))
        : QString();
    unsigned int abrCeiling = effectiveAbrCeiling(m_maxBitrateKbps);
    if (!m_hasSiyiTelemetry)
        abrCeiling = qMin(abrCeiling, m_currentBitrateKbps > 0 ? m_currentBitrateKbps : 100u);
    if (m_isExplicitC2Only)
        abrCeiling = qMin(abrCeiling, m_isStrictVideoCutoff
            ? VIDEO_DISABLED_BITRATE_KBPS : MIN_ACTIVE_VIDEO_BITRATE_KBPS);
    qInfo().noquote() << QString("[%1] RTT: %2ms, BW: %3 Mbps, Loss: %4%, SIYI: %5 | "
                                 "serialConnected: %6, ABR ceiling: %7 kbps, %8State: %9 --> Video: %10 | %11 kbps")
        .arg(QDateTime::currentDateTime().toString("MM-dd HH:mm:ss"))
        .arg(m_latestSmoothedRtt, 0, 'f', 0)
        .arg(m_latestSmoothedBw, 0, 'f', 2)
        .arg(m_latestSmoothedLossPercent, 0, 'f', 1)
        .arg(siyi)
        .arg(m_serialConnected ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(abrCeiling)
        .arg(c2Metrics)
        .arg(state)
        .arg(videoStateLabel)
        .arg(m_currentBitrateKbps);
}

void SRTAdaptiveBitrateStreaming::setCameraControlEnabled(bool enabled)
{
    if (m_cameraControlEnabled == enabled) return;
    m_cameraControlEnabled = enabled;
    m_lastCongestedBitrate = 0;
    m_probePreviousCeiling = 0;
    m_probeStartedAt = 0;
    m_probeHealthySince = 0;
    m_lastProbeTime = 0;
    m_probeRetryDelayMs = FAILURE_MEMORY_PROBE_INTERVAL_MS;
    m_recovery = SRTRecoveryState();
    m_appliedIncreaseSince = 0;
    m_lastAchievedBitrateKbps = 0;
    m_stalledSinceMs = 0;
    m_risingLossSamples = 0;
    m_previousRecoverySampleHadLoss = false;
    m_previousStableBitrate = 0;
    m_wasCongested = false;
}

void SRTAdaptiveBitrateStreaming::handleCameraReportedBitrate(int achievedKbps, int requestedKbps)
{
    if (!m_isRunning) return;
    const unsigned int requested = static_cast<unsigned int>(qMax(0, requestedKbps));
    if (m_cameraControlEnabled && m_lastAchievedBitrateKbps > 0 &&
        requested > m_lastAppliedRequestKbps && m_lastAppliedRequestKbps > 0 &&
        achievedKbps > static_cast<int>(m_lastAchievedBitrateKbps) &&
        canResyncCameraBitrate() && m_latestSmoothedLossPercent == 0.0 &&
        m_latestSmoothedRtt <= qMax(SRTTuning::RECOVERY_LOW_RTT_MS,
            m_rttMin + SRTTuning::RECOVERY_MAX_RAW_RTT_INFLATION_MS)) {
        m_appliedPreviousKbps = m_lastAchievedBitrateKbps;
        m_appliedTargetKbps = static_cast<unsigned int>(achievedKbps);
        m_appliedIncreaseCarriedLoad = false;
        m_appliedIncreaseSince = QDateTime::currentMSecsSinceEpoch();
    } else if (achievedKbps < static_cast<int>(m_lastAchievedBitrateKbps)) {
        m_appliedIncreaseSince = 0;
    }
    m_lastAchievedBitrateKbps = (achievedKbps > 0) ? static_cast<unsigned int>(achievedKbps) : 0;
    m_lastAppliedRequestKbps = requested;
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
    if (!m_isRunning) return;
    if (!stat.isAlive || stat.signal <= 0 || stat.signal > 100 ||
        stat.rssi >= 0 || stat.rssi < -120) {
        handleSiyiLinkLost();
        return;
    }

    const bool firstRfSample = (m_lastSiyiPacketTime == 0);
    const bool recoveringRf = !m_hasSiyiTelemetry;
    if (recoveringRf) {
        // Rebuild RF and QoS confidence while retaining any probe backoff.
        m_siyiRssiHistory.clear();
        m_clearSinceMs = 0;
        m_consecutiveZeroLossCount = 0;
        m_rfRecoveryRampActive = false;
        m_needRecoveryRefresh = true;
        m_latestStatusReason = "RF RESTORED (Waiting for QoS decision)";
        qInfo() << "[RF Environment] Valid SIYI RSSI restored; resuming ABR.";
    }
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

    // Schmitt hysteresis: weaken at the RF boundary immediately; strengthen
    // only 2 dBm above the boundary. Recovery uses the same region state.
    int envIndex = 0;
    while (envIndex < RF_ENVIRONMENT_COUNT - 1 &&
           RF_ENVIRONMENTS[envIndex].ceilingKbps != m_currentEnvCeiling) {
        ++envIndex;
    }
    if (firstRfSample) {
        envIndex = 0;
        while (envIndex < RF_ENVIRONMENT_COUNT - 1 &&
               m_smoothedRssi < RF_ENVIRONMENTS[envIndex].minRssi) {
            ++envIndex;
        }
    } else {
        while (envIndex < RF_ENVIRONMENT_COUNT - 1 &&
               m_smoothedRssi < RF_ENVIRONMENTS[envIndex].minRssi) {
            ++envIndex;
        }
        while (envIndex > 0 &&
               m_smoothedRssi >= RF_ENVIRONMENTS[envIndex - 1].minRssi + RF_ENV_HYSTERESIS_DBM) {
            --envIndex;
        }
    }
    const RfEnvironment &env = RF_ENVIRONMENTS[envIndex];
    // Valid RF returning can accelerate recovery even within the same tier.
    if (recoveringRf && (!firstRfSample || env.ceilingKbps == RF_ENVIRONMENTS[0].ceilingKbps)) {
        m_rfRecoveryRampActive = true;
        m_rfUpgradeFirstStepPending = false;
    }

    // Reconnecting in the same region must preserve the learned safe ceiling.
    if (env.ceilingKbps != m_currentEnvCeiling) {
        m_recovery = SRTRecoveryState();
        m_appliedIncreaseSince = 0;
        m_lastAchievedBitrateKbps = 0;
        m_stalledSinceMs = 0;
        m_risingLossSamples = 0;
        m_previousRawRecoveryRtt = 0.0;
        cancelQosProbe(m_lastSiyiPacketTime, false);
        unsigned int oldCeiling = m_currentEnvCeiling;
        m_currentEnvCeiling = env.ceilingKbps;
        m_currentEnvName = env.name;
        m_rfUpgradeFirstStepPending = m_currentEnvCeiling > oldCeiling;

        if (m_currentEnvCeiling > oldCeiling) {
            // Môi trường mạng MẠNH HƠN (bay lại gần GCS): Nâng trần ngay lập tức, không đợi 30s dò dẫm!
            qInfo().noquote() << QString("[RF Environment] 📶 Entered STRONGER environment: %1 | New Ceiling: %2 kbps (RSSI: %3 dBm)")
                                        .arg(m_currentEnvName).arg(m_currentEnvCeiling).arg(m_smoothedRssi, 0, 'f', 1);

            m_lastCongestedBitrate = 0;
            // Keep an established clear interval across an RF upgrade. The
            // next QoS decision still validates fresh feedback and headroom.
            if (m_clearSinceMs == 0 ||
                m_lastSiyiPacketTime - m_clearSinceMs < CLEAR_STABLE_DURATION_MS) {
                m_clearSinceMs = m_lastSiyiPacketTime;
                m_lastBitrateIncrTime = m_lastSiyiPacketTime;
            }
            m_rfRecoveryRampActive = true;
        } else {
            m_rfRecoveryRampActive = false;
            // Môi trường mạng YẾU HƠN (bay ra xa GCS / khuất sóng): Khóa trần thấp hơn, chủ động hạ bitrate xuống trước khi rớt gói
            qWarning().noquote() << QString("[RF Environment] ⚠️ Entered WEAKER environment: %1 | Lowering Ceiling: %2 kbps (RSSI: %3 dBm)")
                                        .arg(m_currentEnvName).arg(m_currentEnvCeiling).arg(m_smoothedRssi, 0, 'f', 1);

            m_lastCongestedBitrate = 0;

            if (m_currentBitrateKbps > m_currentEnvCeiling) {
                qInfo().noquote() << QString("[RF Environment] Proactively stepping down bitrate from %1 kbps to %2 kbps for link safety")
                                             .arg(m_currentBitrateKbps).arg(m_currentEnvCeiling);
                applyNewBitrate(m_currentEnvCeiling, m_latestSmoothedRtt, m_latestSmoothedBw, 0);
            }
        }
    }
}

void SRTAdaptiveBitrateStreaming::cancelQosProbe(qint64 now, bool failed)
{
    unsigned int rollback = 0;
    if (m_probePreviousCeiling > 0) {
        m_appliedIncreaseSince = 0;
        m_lastCongestedBitrate = m_probePreviousCeiling;
        m_lastProbeTime = now;
        if (failed) {
            m_probeRetryDelayMs = qMin<qint64>(m_probeRetryDelayMs * 2, SRTTuning::FAILURE_MEMORY_PROBE_MAX_BACKOFF_MS);
        }
        qInfo().noquote() << QString("[QoS Engine] QoS probe cancelled; ceiling restored to %1 kbps")
                                 .arg(m_lastCongestedBitrate);
        if (m_currentBitrateKbps > m_lastCongestedBitrate) rollback = m_lastCongestedBitrate;
    }
    m_probePreviousCeiling = 0;
    m_probeStartedAt = 0;
    m_probeHealthySince = 0;
    // Clear the episode before applying: RF-loss handling can re-enter here.
    if (rollback > 0)
        applyNewBitrate(rollback, m_latestSmoothedRtt, m_latestSmoothedBw, 0);
}

void SRTAdaptiveBitrateStreaming::handleSiyiLinkLost()
{
    m_recovery = SRTRecoveryState();
    m_appliedIncreaseSince = 0;
    m_lastAchievedBitrateKbps = 0;
    m_stalledSinceMs = 0;
    m_risingLossSamples = 0;
    m_previousRawRecoveryRtt = 0.0;
    cancelQosProbe(QDateTime::currentMSecsSinceEpoch(), false);
    if (m_hasSiyiTelemetry) {
        qWarning() << "[RF Environment] SIYI RSSI unavailable; increases frozen at"
                   << m_currentBitrateKbps << "kbps until valid RSSI returns (QoS and reductions remain active).";
    }
    m_hasSiyiTelemetry = false;
    m_siyiStat.isAlive = false;
    m_siyiRssiHistory.clear();
    m_clearSinceMs = 0;
    m_consecutiveZeroLossCount = 0;
    m_rfRecoveryRampActive = false;
    m_latestStatusReason = "RF UNAVAILABLE (QoS active; increases frozen)";
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

    QStringList peerIds;
    for (const SRTPeerStat &peer : peers)
        peerIds.append(peer.peerAddress + ':' + QString::number(peer.peerPort));
    peerIds.sort();
    const QString peerSet = peerIds.join(',');
    if (!m_cameraPeerSet.isEmpty() && peerSet != m_cameraPeerSet) {
        m_recovery = SRTRecoveryState();
        m_appliedIncreaseSince = 0;
        m_lastAchievedBitrateKbps = 0;
        m_stalledSinceMs = 0;
        m_risingLossSamples = 0;
        m_previousRawRecoveryRtt = 0.0;
        m_rfRecoveryRampActive = false;
        m_needRecoveryRefresh = true;
        m_clearSinceMs = 0;
        cancelQosProbe(m_lastCameraQosTime, false);
    }
    m_cameraPeerSet = peerSet;

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

unsigned int SRTAdaptiveBitrateStreaming::effectiveAbrCeiling(unsigned int profileCeiling) const
{
    unsigned int ceiling = qMin(profileCeiling, m_bandwidthCapKbps);
    if (m_lastCongestedBitrate > 0) ceiling = qMin(ceiling, m_lastCongestedBitrate);
    if (m_hasSiyiTelemetry && m_siyiStat.isAlive && m_currentEnvCeiling > 0)
        ceiling = qMin(ceiling, m_currentEnvCeiling);
    ceiling = (ceiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
    return qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, ceiling, m_maxBitrateKbps);
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
    if (!m_hasSiyiTelemetry || ctime - m_lastSiyiPacketTime > SIYI_LINK_TIMEOUT_MS) {
        handleSiyiLinkLost();
    }

    const qint64 qosGapMs = m_lastQosPacketTime > 0 ? ctime - m_lastQosPacketTime : 0;
    const bool cameraQosInterrupted = qosGapMs >= CAMERA_QOS_STALE_TIMEOUT_MS;
    if (cameraQosInterrupted) {
        m_cameraQosRecoverySamplesRemaining = CAMERA_QOS_RECOVERY_SAMPLES;
        m_clearSinceMs = 0;
        m_clearBandwidthOverCapSamples = 0;
        cancelQosProbe(ctime, false);
        if (qosGapMs >= CAMERA_QOS_FALLBACK_TIMEOUT_MS &&
            m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS) {
            applyNewBitrate(MIN_ACTIVE_VIDEO_BITRATE_KBPS, 0.0, 0.0, 0);
        }
    }
    const bool isStreamRecovering = m_needRecoveryRefresh || cameraQosInterrupted;
    if (isStreamRecovering) {
        m_previousRecoverySampleHadLoss = false;
        m_recovery.clearPending();
        m_appliedIncreaseSince = 0;
        m_lastAchievedBitrateKbps = 0;
        m_stalledSinceMs = 0;
        m_risingLossSamples = 0;
        m_previousRawRecoveryRtt = 0.0;
        m_needRecoveryRefresh = false;
        m_isVideoCollapsed = false;
        m_consecutivePanicCount = 0;

        m_rttHistory.clear();
        m_bwHistory.clear();
        m_bandwidthCapKbps = m_maxBitrateKbps;
        m_rttWarmupHistory.clear();

        m_hasLastLoss = false;
        m_hasLastSent = false;
        m_hasLastDropSnd = false;
        m_hasLastDropRcv = false;
        m_hasLastRetrans = false;

        m_lossPercentAvg = 0.0;
        m_latestSmoothedLossPercent = 0.0;
        m_consecutiveZeroLossCount = 0;

        m_cooldownUntilMs = ctime + 800;
        qInfo() << "[SRT Recovery] telemetry resumed; gap_ms=" << qosGapMs;
        m_stalledSinceMs = 0;
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
    if (cameraQosInterrupted) {
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
    const bool sentReset = m_hasLastSent && rawSentTotal < m_lastSentTotal;
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

    // Repeated retransmissions with no new traffic are evidence of a stalled stream.
    // Missing telemetry and recovery edges alone do not request a socket restart.
    const bool stalled = m_cameraControlEnabled && !isStreamRecovering &&
        !sentReset && deltaSent == 0 && deltaRetrans > 0 &&
        rawSendRateMbps < 0.02 && m_currentBitrateKbps > MIN_ACTIVE_VIDEO_BITRATE_KBPS &&
        m_hasSiyiTelemetry && m_c2Priority == C2PriorityLevel::Normal;
    if (!stalled) {
        m_stalledSinceMs = 0;
    } else if (m_stalledSinceMs == 0) {
        m_stalledSinceMs = ctime;
    } else if (ctime - m_stalledSinceMs >= SRTTuning::STREAM_STALL_CONFIRM_MS &&
               ctime - m_lastKeyframeRequestTime >= SRTTuning::STREAM_RESTART_INTERVAL_MS) {
        m_lastKeyframeRequestTime = ctime;
        qInfo() << "[requestKeyframe EMIT] reason=confirmed-stall"
                << "ts_ms=" << ctime << "retrans=" << deltaRetrans;
        emit requestKeyframe();
    }

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
        m_previousRecoverySampleHadLoss = false;
        m_recovery = SRTRecoveryState();
        m_appliedIncreaseSince = 0;
        m_lastAchievedBitrateKbps = 0;
        m_risingLossSamples = 0;
        m_previousRawRecoveryRtt = 0.0;
        if (m_cameraQosRecoverySamplesRemaining > 0)
            m_cameraQosRecoverySamplesRemaining = CAMERA_QOS_RECOVERY_SAMPLES;
        m_clearBandwidthOverCapSamples = 0;
        cancelQosProbe(ctime, false);
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

    // Only advancing packet counters can acknowledge a bitrate command.
    // An old send-rate field repeated by the publisher is not fresh feedback.
    const bool feedbackCountersValid = !sentReset && !lossReset &&
        !dropSndReset && !dropRcvReset && !retransReset && !isStreamRecovering;
    const bool freshFeedback = feedbackCountersValid && deltaSent > 0;
    if (sentReset || lossReset || dropSndReset || dropRcvReset || retransReset) {
        m_previousRecoverySampleHadLoss = false;
        m_recovery = SRTRecoveryState();
        m_appliedIncreaseSince = 0;
        m_lastAchievedBitrateKbps = 0;
        m_stalledSinceMs = 0;
        m_risingLossSamples = 0;
        m_previousRawRecoveryRtt = 0.0;
        m_rfRecoveryRampActive = false;
        m_clearSinceMs = 0;
    }
    const bool c2Healthy = m_c2Priority == C2PriorityLevel::Normal &&
        m_c2RateTracker.state != C2BitrateGuard::State::Degraded &&
        m_c2RateTracker.ceiling > 0 &&
        (!m_serialConnected || m_c2RateTracker.hasFreshSample(m_c2RateClock.elapsed()));
    const bool lowRecoveryRtt = rawRtt > 0.0 && rawRtt <= SRTTuning::RECOVERY_LOW_RTT_MS &&
        smoothedRtt <= SRTTuning::RECOVERY_LOW_RTT_MS;
    const bool rawRttInflated = rawRtt > qMax(SRTTuning::RECOVERY_LOW_RTT_MS,
        m_rttMin + SRTTuning::RECOVERY_MAX_RAW_RTT_INFLATION_MS);
    const bool rawRttRising = rawRtt > SRTTuning::RECOVERY_LOW_RTT_MS && m_previousRawRecoveryRtt > 0.0 &&
        rawRtt > m_previousRawRecoveryRtt + SRTTuning::RECOVERY_MAX_RTT_RISE_MS;
    const bool congestionEvidence = state != CongestionState::Clear ||
        (freshFeedback && rawRttInflated);
    const bool repeatedLoss = freshFeedback && deltaFailures > 0 && m_previousRecoverySampleHadLoss;
    if (freshFeedback) m_previousRecoverySampleHadLoss = deltaFailures > 0;
    const bool failedIncrease = congestionEvidence || hasLatencyDrops || repeatedLoss ||
        instantLossPercent > SRTTuning::RECOVERY_TOLERATED_LOSS_PERCENT;
    const bool lossWithinBudget = !failedIncrease &&
        m_lossPercentAvg <= SRTTuning::RECOVERY_TOLERATED_LOSS_PERCENT;
    const bool recoveryLinkHealthy = lossWithinBudget &&
        m_hasSiyiTelemetry && c2Healthy && !m_isExplicitC2Only && !m_isVideoCollapsed &&
        rawRtt > 0.0 && !rawRttInflated && !rawRttRising && (lowRecoveryRtt || rttInflation < 8.0) &&
        rawBandwidthMbps > 0.1 && rawBandwidthMbps < 150.0;
    const bool feedbackHealthy = freshFeedback && recoveryLinkHealthy && deltaFailures == 0;
    const bool recoverySignalsHealthy = recoveryLinkHealthy && m_rttMinSeeded &&
        (lowRecoveryRtt || (rttInflation < SRTTuning::RECOVERY_MAX_SMOOTHED_RTT_INFLATION_MS &&
         m_rttAvgDelta < SRTTuning::RECOVERY_MAX_RTT_TREND_MS));
    const bool recoveryHealthy = feedbackHealthy && recoverySignalsHealthy;
    m_risingLossSamples = freshFeedback && rawRttRising && deltaFailures > 0
        ? m_risingLossSamples + 1 : 0;
    if (freshFeedback && rawRtt > 0.0) m_previousRawRecoveryRtt = rawRtt;

    // A mild isolated loss pauses the increase. Congestion, drops, and
    // meaningful or repeated loss roll back without the normal debounce.
    const bool evaluatingAppliedIncrease = m_cameraControlEnabled &&
        m_appliedIncreaseSince > 0 && ctime - m_appliedIncreaseSince <= SRTTuning::INCREASE_EVALUATION_MS &&
        m_hasSiyiTelemetry && freshFeedback && m_probePreviousCeiling == 0;
    if (evaluatingAppliedIncrease &&
        SRTRecoveryState::carriesIncrease(m_appliedPreviousKbps, m_appliedTargetKbps,
                                          rawSendRateMbps * 1000.0)) {
        m_appliedIncreaseCarriedLoad = true;
    }
    const bool appliedIncreaseFailed = evaluatingAppliedIncrease && failedIncrease &&
        m_appliedIncreaseCarriedLoad && state != CongestionState::Panic &&
        m_lossPercentAvg < LOSS_THRESHOLD_PANIC_PERCENT;
    unsigned int rollback = failedIncrease ? m_recovery.rollbackTarget() : 0;
    if (appliedIncreaseFailed) {
        const unsigned int safe = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS,
            (m_appliedPreviousKbps / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS,
            m_maxBitrateKbps);
        unsigned int rollbackCeiling = safe;
        // Require congestion, drops, or multiple failures to learn a ceiling.
        if (congestionEvidence || hasLatencyDrops || repeatedLoss || deltaFailures > 1) {
            m_lastCongestedBitrate = m_lastCongestedBitrate > 0
                ? qMin(m_lastCongestedBitrate, safe) : safe;
            rollbackCeiling = m_lastCongestedBitrate;
            m_lastProbeTime = ctime;
            qInfo() << "[SRT Ceiling] learned_kbps=" << m_lastCongestedBitrate
                    << "reason=applied-increase-failed";
        }
        if (m_currentBitrateKbps > rollbackCeiling)
            rollback = rollback > 0 ? qMin(rollback, rollbackCeiling) : rollbackCeiling;
        m_appliedIncreaseSince = 0;
    }
    if (rollback > 0) {
        m_rfRecoveryRampActive = false;
        if (m_probePreviousCeiling > 0)
            cancelQosProbe(ctime, true);
        if (rollback < m_currentBitrateKbps)
            applyNewBitrate(rollback, smoothedRtt, smoothedBw, deltaLoss);
        m_clearSinceMs = 0;
        m_cooldownUntilMs = qMax(m_cooldownUntilMs, ctime + RF_RECOVERY_INTERVAL_MS);
        qInfo() << "[QoS Engine] Unconfirmed increase rolled back to" << rollback << "kbps"
                << "failures=" << deltaFailures << "state=" << static_cast<int>(state)
                << "raw_rtt_ms=" << rawRtt << "base_rtt_ms=" << m_rttMin
                << "fresh=" << freshFeedback << "send_mbps=" << rawSendRateMbps;
    }
    m_recovery.observe(ctime, m_currentBitrateKbps, rawSendRateMbps * 1000.0, feedbackHealthy);
    if (feedbackHealthy && m_recovery.feedbackTimedOut(ctime)) {
        m_recovery.clearPending();
        qInfo() << "[SRT Recovery] increase feedback timed out; holding" << m_currentBitrateKbps << "kbps";
        m_lastBitrateIncrTime = ctime;
    }

    // An isolated mild loss or unchanged counters can preserve an existing
    // probe, but only fresh, clean feedback carrying load can advance it.
    const bool probeCarriesLoad =
        SRTRecoveryState::carriesTarget(m_currentBitrateKbps, rawSendRateMbps * 1000.0) &&
        (m_probePreviousCeiling == 0 || SRTRecoveryState::carriesIncrease(
            m_probePreviousCeiling, m_lastCongestedBitrate, rawSendRateMbps * 1000.0));
    const bool probeHealthy = recoveryHealthy && probeCarriesLoad;
    const bool probeSignalsHealthy = feedbackCountersValid && recoverySignalsHealthy;
    const bool duplicateFeedback = deltaSent == 0 && deltaFailures == 0;
    if (m_probePreviousCeiling > 0 && freshFeedback && !probeCarriesLoad) m_probeStartedAt = 0;
    if (!probeSignalsHealthy || (!freshFeedback && !duplicateFeedback) ||
        (freshFeedback && !probeCarriesLoad && m_probePreviousCeiling == 0)) {
        const bool probing = m_probePreviousCeiling > 0;
        if (probing) {
            qInfo() << "[SRT Probe] unhealthy feedback; fresh=" << freshFeedback
                    << "rtt_rising=" << rawRttRising << "raw_rtt_ms=" << rawRtt
                    << "base_rtt_ms=" << m_rttMin << "failures=" << deltaFailures
                    << "loss_pct=" << m_lossPercentAvg << "c2_healthy=" << c2Healthy;
        }
        cancelQosProbe(ctime, probing && (failedIncrease || (freshFeedback && rawRttRising)));
    } else if (m_probePreviousCeiling > 0 &&
               ctime - m_lastProbeTime >= SRTTuning::FAILURE_MEMORY_PROBE_TIMEOUT_MS) {
        qInfo() << "[SRT Ceiling] probe timed out without confirmation";
        cancelQosProbe(ctime, false);
    } else if (probeHealthy && m_probeHealthySince == 0) {
        m_probeHealthySince = ctime;
    }

    if (m_risingLossSamples >= 2 && state != CongestionState::Panic &&
        rawRttInflated &&
        ctime - m_lastBitrateChangeTime >= BITRATE_DECR_NORMAL_INTERVAL_MS) {
        const unsigned int target = qMax(MIN_ACTIVE_VIDEO_BITRATE_KBPS,
                                         m_currentBitrateKbps * 85 / 100);
        qInfo() << "[SRT Decision] reason=rising-rtt-with-loss"
                << "rtt_ms=" << rawRtt << "failures=" << deltaFailures
                << "target_kbps=" << target;
        applyNewBitrate(target, smoothedRtt, smoothedBw, deltaLoss);
        m_cooldownUntilMs = ctime + RF_RECOVERY_INTERVAL_MS;
        m_clearSinceMs = 0;
        m_risingLossSamples = 0;
        return;
    }

    if (state == CongestionState::Panic) {
        if (ctime - m_lastCollapseLogTime > 2000) {
            m_lastCollapseLogTime = ctime;
        }
    }

    if (failedIncrease) {
        m_rfRecoveryRampActive = false;
    }
    if (cameraQosInterrupted) {
        m_rfRecoveryRampActive = false;
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

    m_bandwidthCapKbps = bandwidthCapKbps;

    const bool clearAboveBandwidthCap = state == CongestionState::Clear
        && rawBandwidthMbps > 0.1 && rawBandwidthMbps < 150.0
        && rawBandwidthMbps * 1000.0 * BW_UTILIZATION_RATIO < m_currentBitrateKbps
        && m_currentBitrateKbps > bandwidthCapKbps;
    if (!clearAboveBandwidthCap) {
        m_clearBandwidthOverCapSamples = 0;
    }

    if (!m_isBootstrapped) {
        // The first RTT sample also seeds the baseline, so check its
        // absolute value before treating CLEAR as a healthy startup.
        constexpr double maxStartupRttMs = 60.0;
        const bool canStartFast = state == CongestionState::Clear && m_hasSiyiTelemetry &&
                                  m_c2RateTracker.state != C2BitrateGuard::State::Degraded &&
                                  m_c2RateTracker.ceiling > 0 && hasActiveTraffic &&
                                  rawRtt > 0.0 && rawRtt <= maxStartupRttMs &&
                                  rawBandwidthMbps > 0.1 && rawBandwidthMbps < 150.0;
        unsigned int calculatedInitialBitrate = canStartFast ? SRTTuning::STARTUP_BITRATE_KBPS : 100u;
        if (state == CongestionState::Panic)
            calculatedInitialBitrate = MIN_ACTIVE_VIDEO_BITRATE_KBPS;

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
        m_latestStatusReason = "MODERATE (1.5M)";
    } else if (state == CongestionState::Severe) {
        m_latestStatusReason = "SEVERE (0.7M)";
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
        targetProfileBitrate = 700;
        requiredInterval = BITRATE_DECR_FAST_INTERVAL_MS;
        break;
    case CongestionState::Moderate:
        targetProfileBitrate = 1500;
        requiredInterval = BITRATE_DECR_NORMAL_INTERVAL_MS;
        break;
    case CongestionState::Clear:
    default:
        targetProfileBitrate = m_maxBitrateKbps;
        requiredInterval = BITRATE_INCR_DECISION_INTERVAL_MS;
        break;
    }

    if (m_cameraQosRecoverySamplesRemaining > 0) {
        const bool validSample = hasActiveTraffic && rawRtt > 0.0 &&
                                 rawBandwidthMbps > 0.0 && rawBandwidthMbps <= 150.0;
        if (state != CongestionState::Clear) {
            const unsigned int target = qMin(targetProfileBitrate, bandwidthCapKbps);
            if (target < m_currentBitrateKbps)
                applyNewBitrate(target, smoothedRtt, smoothedBw, deltaLoss);
        }
        if (isStreamRecovering || !validSample || state != CongestionState::Clear || deltaFailures > 0) {
            m_cameraQosRecoverySamplesRemaining = CAMERA_QOS_RECOVERY_SAMPLES;
            return;
        }
        if (--m_cameraQosRecoverySamplesRemaining == 0) {
            const unsigned int warmStart = qMax(CAMERA_QOS_WARM_START_MIN_KBPS, m_currentBitrateKbps / 2);
            const unsigned int target = qMin(m_currentBitrateKbps, qMin(warmStart, bandwidthCapKbps));
            m_clearSinceMs = ctime;
            m_lastBitrateChangeTime = ctime;
            m_lastBitrateIncrTime = ctime;
            m_wasCongested = false;
            m_rfRecoveryRampActive = m_hasSiyiTelemetry &&
                m_currentEnvCeiling == RF_ENVIRONMENTS[0].ceilingKbps;
            m_rfUpgradeFirstStepPending = false;
            applyNewBitrate(target, smoothedRtt, smoothedBw, deltaLoss);
        }
        return;
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

        }

        if (clearAboveBandwidthCap) {
            m_rfRecoveryRampActive = false;
            // Confirm with consecutive fresh samples before reacting to an estimate dip.
            m_clearBandwidthOverCapSamples = qMin(m_clearBandwidthOverCapSamples + 1, 2);
            if (m_clearBandwidthOverCapSamples >= 2 &&
                timeSinceLastChange >= BITRATE_DECR_NORMAL_INTERVAL_MS) {
                const unsigned int target = m_probePreviousCeiling > 0
                    ? qMin(bandwidthCapKbps, m_probePreviousCeiling) : bandwidthCapKbps;
                cancelQosProbe(ctime, false);
                m_lastBitrateChangeTime = ctime;
                m_lastBitrateIncrTime = ctime;
                m_clearSinceMs = ctime;
                m_clearBandwidthOverCapSamples = 0;
                applyNewBitrate(target, smoothedRtt, smoothedBw, deltaLoss);
            }
            return;
        }

        bool isBufferDrained = lowRecoveryRtt || rttInflation < 8.0 ||
            (m_rttMin > 0.0 && smoothedRtt <= m_rttMin * 1.30 + 5.0);

        bool cooldownExpired = (ctime >= m_cooldownUntilMs);

        // Recover quickly below a recently demonstrated rate, or after a real
        // RF upgrade. Discovery outside that range keeps the slower cadence.
        const unsigned int provenTarget = m_recovery.provenTarget(ctime);
        const bool fastRecovery = recoveryHealthy &&
            (m_rfRecoveryRampActive || provenTarget > m_currentBitrateKbps);
        unsigned int recoveryTarget = bandwidthCapKbps;
        if (fastRecovery) {
            const auto rawCap = static_cast<unsigned int>(rawBandwidthMbps * 1000.0 * BW_UTILIZATION_RATIO);
            recoveryTarget = qMin(recoveryTarget, rawCap);
            recoveryTarget = qMin(recoveryTarget, static_cast<unsigned int>(m_c2RateTracker.ceiling));
            if (m_lastCongestedBitrate > 0) recoveryTarget = qMin(recoveryTarget, m_lastCongestedBitrate);
            if (!m_rfRecoveryRampActive) recoveryTarget = qMin(recoveryTarget, provenTarget);
        }
        const qint64 availableHeadroom = qMax<qint64>(0,
            static_cast<qint64>(recoveryTarget) - m_currentBitrateKbps);
        const bool rfRecoveryProbe = m_rfRecoveryRampActive && m_lastCongestedBitrate > 0;
        const qint64 incrDecisionInterval = fastRecovery &&
            ((rfRecoveryProbe && m_currentBitrateKbps >= m_lastCongestedBitrate) ||
             availableHeadroom >= SRTTuning::RECOVERY_FAST_HEADROOM_KBPS)
            ? RF_RECOVERY_INTERVAL_MS : BITRATE_INCR_DECISION_INTERVAL_MS;
        bool decisionIntervalExpired = (ctime - m_lastBitrateIncrTime >= incrDecisionInterval);
        bool clearStableMet = (ctime - m_clearSinceMs >= CLEAR_STABLE_DURATION_MS);

        if (cooldownExpired && decisionIntervalExpired && clearStableMet) {
            if (!freshFeedback || m_recovery.awaitingFeedback() || rawRttRising ||
                rawRtt <= 0.0 || rawRttInflated) {
                m_latestStatusReason = "CLEAR (Waiting for bitrate feedback / RTT settling)";
                if (ctime - m_lastDecisionLogMs >= 3000) {
                    m_lastDecisionLogMs = ctime;
                    qInfo() << "[SRT Hold] fresh=" << freshFeedback
                            << "awaiting_load=" << m_recovery.awaitingFeedback()
                            << "rtt_rising=" << rawRttRising << "raw_rtt_ms=" << rawRtt
                            << "base_rtt_ms=" << m_rttMin << "send_mbps=" << rawSendRateMbps
                            << "learned_cap=" << m_lastCongestedBitrate
                            << "rf_cap=" << m_currentEnvCeiling << "bw_cap=" << bandwidthCapKbps;
                }
                return;
            }
            if (!isBufferDrained) {
                m_latestStatusReason = QString("CLEAR (Holding - RTT %1ms/Base %2ms)").arg(smoothedRtt, 0, 'f', 0).arg(m_rttMin, 0, 'f', 0);
                return;
            }

            // A learned QoS ceiling is temporary: wait for continuous clean
            // traffic, then test one small step and confirm it before committing.
            if (probeHealthy) {
                if (m_probePreviousCeiling > 0 && m_currentBitrateKbps >= m_lastCongestedBitrate) {
                    if (m_probeStartedAt == 0) m_probeStartedAt = ctime;
                    if (ctime - m_probeStartedAt >= FAILURE_MEMORY_PROBE_CONFIRM_MS) {
                        m_probePreviousCeiling = 0;
                        m_appliedIncreaseSince = 0;
                        m_probeStartedAt = 0;
                        m_probeRetryDelayMs = FAILURE_MEMORY_PROBE_INTERVAL_MS;
                        m_lastProbeTime = ctime;
                        qInfo().noquote() << QString("[QoS Engine] QoS probe confirmed: %1 kbps")
                                                 .arg(m_lastCongestedBitrate);
                    }
                }
                const unsigned int rawCap = static_cast<unsigned int>(rawBandwidthMbps * 1000.0 * BW_UTILIZATION_RATIO);
                const unsigned int probeCap = qMin(qMin(qMin(bandwidthCapKbps, m_currentEnvCeiling), rawCap),
                    static_cast<unsigned int>(m_c2RateTracker.ceiling));
                const qint64 probeHealthDuration = rfRecoveryProbe
                    ? CLEAR_STABLE_DURATION_MS : FAILURE_MEMORY_RETENTION_MS;
                const qint64 probeRetryDelay = rfRecoveryProbe &&
                    m_probeRetryDelayMs == FAILURE_MEMORY_PROBE_INTERVAL_MS
                    ? CLEAR_STABLE_DURATION_MS : m_probeRetryDelayMs;
                if (m_probePreviousCeiling == 0 && m_lastCongestedBitrate > 0
                    && m_currentBitrateKbps == m_lastCongestedBitrate
                    && probeCap >= m_lastCongestedBitrate + FAILURE_MEMORY_PROBE_STEP_KBPS
                    && ctime - m_probeHealthySince >= probeHealthDuration
                    && ctime - m_lastProbeTime >= probeRetryDelay) {
                    m_probePreviousCeiling = m_lastCongestedBitrate;
                    const unsigned int maxProbeStep = m_probeRetryDelayMs > FAILURE_MEMORY_PROBE_INTERVAL_MS
                        ? SRTTuning::FAILURE_MEMORY_PROBE_RETRY_MAX_STEP_KBPS
                        : SRTTuning::FAILURE_MEMORY_PROBE_MAX_STEP_KBPS;
                    const unsigned int probeStep = qMin(probeCap - m_lastCongestedBitrate,
                        qBound(FAILURE_MEMORY_PROBE_STEP_KBPS,
                               m_lastCongestedBitrate / 2, maxProbeStep));
                    const unsigned int probeTarget = m_lastCongestedBitrate + probeStep;
                    m_lastCongestedBitrate = (probeTarget / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
                    m_probeStartedAt = 0;
                    m_lastProbeTime = ctime;
                    qInfo().noquote() << QString("[QoS Engine] QoS probe: %1 -> %2 kbps%3")
                                             .arg(m_probePreviousCeiling).arg(m_lastCongestedBitrate)
                                             .arg(rfRecoveryProbe ? " (RF recovery)" : "");
                }
            }

            // A learned ceiling can only be lifted by the single bounded probe
            // above, or an RF upgrade. Recovery memory cannot bypass it.
            const unsigned int effectiveMax = effectiveAbrCeiling(targetProfileBitrate);

            if (m_currentBitrateKbps >= effectiveMax) {
                if (!rfRecoveryProbe) m_rfRecoveryRampActive = false;
                m_latestStatusReason = QString("CLEAR (LOCKED at %1 | %2k)")
                                           .arg(m_hasSiyiTelemetry ? m_currentEnvName : "Ceiling")
                                           .arg(effectiveMax);
                return;
            } else {
                // Wait for a clean current sample. Residual mild loss in the
                // moving average does not block recovery.
                if (deltaFailures > 0 || !lossWithinBudget) {
                    m_latestStatusReason = QString("CLEAR (Holding - Loss %1%)").arg(m_lossPercentAvg, 0, 'f', 1);
                    return;
                }

                // Smooth and gradual ramp-up (fast recovery when far below capacity, gentle near ceiling)
                const unsigned int headroom = effectiveMax - m_currentBitrateKbps;
                unsigned int stepKbps = BITRATE_ROUNDING_STEP_KBPS;
                if (headroom >= SRTTuning::DISCOVERY_LARGE_HEADROOM_KBPS) {
                    stepKbps = SRTTuning::DISCOVERY_LARGE_STEP_KBPS;
                } else if (headroom >= SRTTuning::DISCOVERY_MEDIUM_HEADROOM_KBPS) {
                    stepKbps = SRTTuning::DISCOVERY_MEDIUM_STEP_KBPS;
                }
                unsigned int targetBitrate = m_currentBitrateKbps + stepKbps;
                targetBitrate = qMin(targetBitrate, effectiveMax);
                if (m_probePreviousCeiling > 0)
                    targetBitrate = qMin(effectiveMax, m_lastCongestedBitrate);
                if (fastRecovery && m_probePreviousCeiling == 0) {
                    const unsigned int recoveryCap = qMin(effectiveMax, recoveryTarget);
                    targetBitrate = m_currentBitrateKbps;
                    if (recoveryCap > m_currentBitrateKbps) {
                        const bool firstRfUpgradeStep = m_rfRecoveryRampActive && m_rfUpgradeFirstStepPending &&
                            (!m_serialConnected || m_c2RateTracker.state == C2BitrateGuard::State::Healthy);
                        targetBitrate = m_currentBitrateKbps +
                            SRTRecoveryState::step(m_currentBitrateKbps, recoveryCap, firstRfUpgradeStep);
                        qInfo() << "[QoS Engine] Fast recovery:" << m_currentBitrateKbps
                                << "->" << targetBitrate << "kbps; target" << recoveryCap
                                << "first_rf_upgrade_step=" << firstRfUpgradeStep;
                    }
                }
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
        cancelQosProbe(ctime, m_probePreviousCeiling > 0);

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

            // Select a bounded reduction target independently of learned capacity.
            unsigned int safeCeiling = previousSafe;
            safeCeiling = (safeCeiling / BITRATE_ROUNDING_STEP_KBPS) * BITRATE_ROUNDING_STEP_KBPS;
            safeCeiling = qBound(MIN_ACTIVE_VIDEO_BITRATE_KBPS, safeCeiling, m_maxBitrateKbps);

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
                immediateTarget = qMin(immediateTarget, bandwidthCapKbps);
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

    // Without RF telemetry, allow a low QoS-driven bootstrap and reductions,
    // but never increase an already running stream. Do not invent an RSSI.
    // This applies to every writer, including C2 and watchdog paths.
    if (!m_hasSiyiTelemetry ||
        QDateTime::currentMSecsSinceEpoch() - m_lastSiyiPacketTime > SIYI_LINK_TIMEOUT_MS) {
        handleSiyiLinkLost();
        const unsigned int ceiling = m_currentBitrateKbps > 0
            ? m_currentBitrateKbps : 100u;
        targetBitrateKbps = qMin(targetBitrateKbps, ceiling);
    }

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
            m_rfUpgradeFirstStepPending = false;
            m_recovery = SRTRecoveryState();
            m_appliedIncreaseSince = 0;
            m_lastAchievedBitrateKbps = 0;
            m_stalledSinceMs = 0;
            m_risingLossSamples = 0;
            m_lastBitrateChangeTime = QDateTime::currentMSecsSinceEpoch();
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

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_rfUpgradeFirstStepPending = false;
    if (m_cameraControlEnabled)
        m_recovery.bitrateChanged(now, m_currentBitrateKbps, targetBitrateKbps);
    m_lastBitrateChangeTime = now;
    if (targetBitrateKbps > m_currentBitrateKbps) m_lastBitrateIncrTime = now;
    m_currentBitrateKbps = targetBitrateKbps;
    emit bitrateChanged(m_currentBitrateKbps);
}
