#ifndef SRTADAPTIVEBITRATESTREAMING_H
#define SRTADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QVector>
#include <QtMath>
#include "IAdaptiveBitrateStreaming.h"
#include "SRTPeerStat.h"
#include "ABRConfigs.h"
#include "SiyiLinkStat.h"
#include "C2BitrateGuard.h"
#include "SRTRecoveryState.h"
#include <QElapsedTimer>

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    // Values live in SRTTuning.h; aliases preserve the existing public API.
    static constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS = SRTTuning::DEFAULT_MIN_BITRATE_KBPS;
    static constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS = SRTTuning::DEFAULT_MAX_BITRATE_KBPS;
    static constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS = SRTTuning::DEFAULT_INITIAL_BITRATE_KBPS;
    static constexpr unsigned int MIN_ACTIVE_VIDEO_BITRATE_KBPS = SRTTuning::MIN_ACTIVE_VIDEO_BITRATE_KBPS;
    static constexpr qint64 BITRATE_INCR_DECISION_INTERVAL_MS = SRTTuning::BITRATE_INCR_DECISION_INTERVAL_MS;
    static constexpr qint64 BITRATE_DECR_FAST_INTERVAL_MS = SRTTuning::BITRATE_DECR_FAST_INTERVAL_MS;
    static constexpr qint64 BITRATE_DECR_NORMAL_INTERVAL_MS = SRTTuning::BITRATE_DECR_NORMAL_INTERVAL_MS;
    static constexpr qint64 RECOVERY_COOLDOWN_MS = SRTTuning::RECOVERY_COOLDOWN_MS;
    static constexpr qint64 CLEAR_STABLE_DURATION_MS = SRTTuning::CLEAR_STABLE_DURATION_MS;
    static constexpr unsigned int RF_RECOVERY_STEP_PERCENT = SRTTuning::RF_RECOVERY_STEP_PERCENT;
    static constexpr unsigned int RF_RECOVERY_MAX_STEP_KBPS = SRTTuning::RF_RECOVERY_MAX_STEP_KBPS;
    static constexpr qint64 RF_RECOVERY_INTERVAL_MS = SRTTuning::RF_RECOVERY_INTERVAL_MS;
    static constexpr int SLIDING_WINDOW_SIZE = SRTTuning::SLIDING_WINDOW_SIZE;
    static constexpr double MIN_VALID_RTT_MS = SRTTuning::MIN_VALID_RTT_MS;
    static constexpr double BW_UTILIZATION_RATIO = SRTTuning::BW_UTILIZATION_RATIO;
    static constexpr unsigned int FAILURE_MEMORY_PROBE_STEP_KBPS = SRTTuning::FAILURE_MEMORY_PROBE_STEP_KBPS;
    static constexpr qint64 FAILURE_MEMORY_PROBE_INTERVAL_MS = SRTTuning::FAILURE_MEMORY_PROBE_INTERVAL_MS;
    static constexpr qint64 FAILURE_MEMORY_RETENTION_MS = SRTTuning::FAILURE_MEMORY_RETENTION_MS;
    static constexpr qint64 FAILURE_MEMORY_PROBE_CONFIRM_MS = SRTTuning::FAILURE_MEMORY_PROBE_CONFIRM_MS;
    static constexpr int RTT_BASELINE_WARMUP_SAMPLES = SRTTuning::RTT_BASELINE_WARMUP_SAMPLES;
    static constexpr double RTT_BASELINE_DRIFT_PER_SEC = SRTTuning::RTT_BASELINE_DRIFT_PER_SEC;
    static constexpr unsigned int BITRATE_ROUNDING_STEP_KBPS = SRTTuning::BITRATE_ROUNDING_STEP_KBPS;
    static constexpr qint64 KEYFRAME_REQUEST_COOLDOWN_MS = SRTTuning::KEYFRAME_REQUEST_COOLDOWN_MS;
    static constexpr qint64 KEYFRAME_IMMUNITY_DURATION_MS = SRTTuning::KEYFRAME_IMMUNITY_DURATION_MS;
    static constexpr qint64 MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE = SRTTuning::MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE;
    static constexpr qint64 CAMERA_QOS_STALE_TIMEOUT_MS = SRTTuning::CAMERA_QOS_STALE_TIMEOUT_MS;
    static constexpr qint64 CAMERA_QOS_FALLBACK_TIMEOUT_MS = SRTTuning::CAMERA_QOS_FALLBACK_TIMEOUT_MS;
    static constexpr int CAMERA_QOS_RECOVERY_SAMPLES = SRTTuning::CAMERA_QOS_RECOVERY_SAMPLES;
    static constexpr unsigned int CAMERA_QOS_WARM_START_MIN_KBPS = SRTTuning::CAMERA_QOS_WARM_START_MIN_KBPS;
    static constexpr qint64 TRAFFIC_IDLE_TIMEOUT_MS = SRTTuning::TRAFFIC_IDLE_TIMEOUT_MS;
    static constexpr double LOSS_THRESHOLD_BOTTLENECK_PERCENT = SRTTuning::LOSS_THRESHOLD_BOTTLENECK_PERCENT;
    static constexpr double LOSS_THRESHOLD_SEVERE_PERCENT = SRTTuning::LOSS_THRESHOLD_SEVERE_PERCENT;
    static constexpr double LOSS_THRESHOLD_PANIC_PERCENT = SRTTuning::LOSS_THRESHOLD_PANIC_PERCENT;
    static constexpr qint64 TRANSIENT_OCCLUSION_DEBOUNCE_MS = SRTTuning::TRANSIENT_OCCLUSION_DEBOUNCE_MS;

    enum class CongestionState {
        Clear = 0,     
        Moderate,     
        Severe,         
        Panic           
    };

    enum class C2Quality {
        Offline = 0,
        Critical,
        Poor,
        Fair,
        Good,
        Excellent
    };

    enum class C2PriorityLevel {
        Normal = 0,
        High,
        Critical,
        C2_Only
    };

    explicit SRTAdaptiveBitrateStreaming(QObject *parent = nullptr);
    void start() override;
    void stop() override;
    void reset(int bitrateKbps) override;
    void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) override;
    bool isTrafficActive() const override { return !m_isTrafficIdle; }

    bool canResyncCameraBitrate() const;
    unsigned int currentBitrate() const { return m_currentBitrateKbps; }

public slots:
    void handleQosCameraConnection(const QVector<SRTPeerStat> &peers);
    void handleC2ConnectionStats(const QVector<SRTPeerStat> &peers);
    void setSerialConnected(bool connected);
    void handleSiyiLinkStat(const SiyiLinkStat &stat);
    void handleSiyiLinkLost();
    void onHeartbeatTimeout();
    void handleCameraReportedBitrate(int achievedKbps, int requestedKbps = 0);
    void setCameraControlEnabled(bool enabled);

private:
    friend class SrtRecoveryTest;
    CongestionState classifyCongestion(double lossPercent, double rtt, double rttInflation, bool useExitThresholds, bool hasLatencyDrops = false, bool isKeyframeBurst = false) const;

    void processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps,
                       int rawLossTotal, qint64 rawSentTotal = 0,
                       int rawDropSndTotal = 0, int rawDropRcvTotal = 0,
                       int rawRetransTotal = 0);
    void applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss);
    unsigned int calculateNetworkPartitionCeiling(double smoothedBw, double lossPercent, CongestionState state) const;

    unsigned int effectiveAbrCeiling(unsigned int profileCeiling) const;
    // Exact bandwidth/goodput cap from the latest QoS decision, before learned limits.
    unsigned int m_bandwidthCapKbps = DEFAULT_MAX_BITRATE_KBPS;

    // SIYI RF Environment helper
    const RfEnvironment& getRfEnvironment(double rssi) const;

    void evaluateC2Quality();
    QString c2PriorityToString(C2PriorityLevel p) const;

    double calculateMedian(QVector<double> list);
    double calculateAverage(const QVector<double> &list);

    bool m_isRunning;

    unsigned int m_currentBitrateKbps;
    unsigned int m_minBitrateKbps;
    unsigned int m_maxBitrateKbps;

    // C2 Telemetry & Priority State
    C2Quality m_c2Quality;
    C2PriorityLevel m_c2Priority;
    bool m_isVideoEnabled;
    bool m_isExplicitC2Only;
    bool m_isStrictVideoCutoff;
    double m_c2Rtt;
    double m_c2Retransmits; // packets per second, not lifetime counters
    double m_c2Unacked;
    QElapsedTimer m_c2RateClock;
    C2BitrateGuard m_c2RateTracker;
    bool m_serialConnected = false;
    qint64 m_lastC2PacketTime;

    // Sliding window sample histories
    QVector<double> m_rttHistory;
    QVector<double> m_bwHistory;

    double m_rttAvg;
    double m_rttAvgDelta;
    double m_prevRtt;
    double m_rttMin;
    QVector<double> m_rttWarmupHistory;
    bool m_rttMinSeeded;

    qint64 m_lastBitrateChangeTime;
    qint64 m_lastBitrateIncrTime;
    qint64 m_cooldownUntilMs;
    int m_consecutiveZeroLossCount;
    qint64 m_clearSinceMs;
    unsigned int m_previousStableBitrate;
    unsigned int m_lastCongestedBitrate;
    qint64 m_lastProbeTime;
    qint64 m_probeHealthySince;
    qint64 m_probeStartedAt;
    qint64 m_probeRetryDelayMs;
    unsigned int m_probePreviousCeiling;
    void cancelQosProbe(qint64 now, bool failed);
    double m_bottleneckBwMbps;

    QTimer *m_heartbeatTimer;
    double m_latestSmoothedRtt;
    double m_latestSmoothedBw;
    int m_latestDeltaLoss;
    double m_lossPercentAvg;
    double m_latestSmoothedLossPercent;
    QString m_latestStatusReason;
    qint64 m_lastQosPacketTime;

    int m_lastLossTotal;
    bool m_hasLastLoss;
    qint64 m_lastSentTotal;
    bool m_hasLastSent;
    int m_lastDropSndTotal;
    bool m_hasLastDropSnd;
    int m_lastDropRcvTotal;
    bool m_hasLastDropRcv;
    int m_lastRetransTotal;
    bool m_hasLastRetrans;
    bool m_isBootstrapped;
    CongestionState m_lastCongestionState;
    bool m_wasCongested;
    qint64 m_lastKeyframeRequestTime;
    int m_consecutivePanicCount;
    qint64 m_congestionStartTimeMs;
    int m_consecutiveCongestedSamples;
    int m_clearBandwidthOverCapSamples = 0;
    bool m_rfRecoveryRampActive;
    bool m_rfUpgradeFirstStepPending = false;
    SRTRecoveryState m_recovery;
    double m_previousRawRecoveryRtt = 0.0;
    QString m_cameraPeerSet;

    qint64 m_lastCameraQosTime;
    qint64 m_lastActiveTrafficTime;
    bool m_isTrafficIdle;
    bool m_isVideoCollapsed;
    bool m_needRecoveryRefresh;
    int m_cameraQosRecoverySamplesRemaining = 0;
    qint64 m_lastCollapseLogTime;
    unsigned int m_lastAchievedBitrateKbps;
    bool m_cameraControlEnabled = true;
    qint64 m_appliedIncreaseSince = 0;
    unsigned int m_appliedPreviousKbps = 0;
    unsigned int m_appliedTargetKbps = 0;
    bool m_appliedIncreaseCarriedLoad = false;
    unsigned int m_lastAppliedRequestKbps = 0;
    qint64 m_stalledSinceMs = 0;
    int m_risingLossSamples = 0;
    bool m_previousRecoverySampleHadLoss = false;
    qint64 m_lastDecisionLogMs = 0;

    // SIYI HM30 RF Telemetry & Environment State
    SiyiLinkStat m_siyiStat;
    QVector<double> m_siyiRssiHistory;
    double m_smoothedRssi;
    int m_smoothedSignal;
    qint64 m_lastSiyiPacketTime;
    unsigned int m_currentEnvCeiling;
    QString m_currentEnvName;
    bool m_hasSiyiTelemetry;
};

#endif // SRTADAPTIVEBITRATESTREAMING_H
