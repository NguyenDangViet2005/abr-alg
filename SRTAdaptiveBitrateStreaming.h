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

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    // ── BelaCoder Configuration Constants ──
    static constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS          = 0;
    static constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS          = 6000;
    static constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS      = 6000;
    static constexpr unsigned int MIN_ACTIVE_VIDEO_BITRATE_KBPS     = 50;

    static constexpr qint64 BITRATE_INCR_DECISION_INTERVAL_MS       = 500;  
    static constexpr qint64 BITRATE_DECR_FAST_INTERVAL_MS           = 250; 
    static constexpr qint64 BITRATE_DECR_NORMAL_INTERVAL_MS         = 400;  
    static constexpr qint64 RECOVERY_COOLDOWN_MS                    = 1500;
 
    static constexpr qint64 CLEAR_STABLE_DURATION_MS                = 500;
    static constexpr int SLIDING_WINDOW_SIZE                        = 5;
    static constexpr double MIN_VALID_RTT_MS                        = 5.0;

    static constexpr double BW_UTILIZATION_RATIO                    = 0.90;

    static constexpr unsigned int FAILURE_MEMORY_PROBE_STEP_KBPS    = 100;
    static constexpr qint64 FAILURE_MEMORY_PROBE_INTERVAL_MS        = 10000;
    static constexpr qint64 FAILURE_MEMORY_RETENTION_MS             = 30000;

    static constexpr int RTT_BASELINE_WARMUP_SAMPLES                = 5;

    static constexpr double RTT_BASELINE_DRIFT_PER_SEC              = 0.002;

    static constexpr unsigned int BITRATE_ROUNDING_STEP_KBPS        = 50;
    static constexpr qint64 KEYFRAME_REQUEST_COOLDOWN_MS            = 10000;
    static constexpr qint64 KEYFRAME_IMMUNITY_DURATION_MS           = 800;
    static constexpr qint64 MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE      = 20;

    static constexpr qint64 CAMERA_QOS_STALE_TIMEOUT_MS             = 3000;
    static constexpr qint64 TRAFFIC_IDLE_TIMEOUT_MS                 = 2500;

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

public slots:
    void handleQosCameraConnection(const QVector<SRTPeerStat> &peers);
    void handleC2ConnectionStats(const QVector<SRTPeerStat> &peers);
    void onHeartbeatTimeout();
    void handleCameraReportedBitrate(int achievedKbps);

private:
    CongestionState classifyCongestion(double lossPercent, double rtt, double rttInflation, bool useExitThresholds, bool hasLatencyDrops = false, bool isKeyframeBurst = false) const;

    void processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps,
                       int rawLossTotal, qint64 rawSentTotal = 0,
                       int rawDropSndTotal = 0, int rawDropRcvTotal = 0,
                       int rawRetransTotal = 0);
    void applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss);

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
    int m_c2Retransmits;
    int m_c2Unacked;
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
    unsigned int m_lastCongestedBitrate;
    qint64 m_lastProbeTime;

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

    qint64 m_lastCameraQosTime;
    qint64 m_lastActiveTrafficTime;
    bool m_isTrafficIdle;
    bool m_isVideoCollapsed;
    bool m_needRecoveryRefresh;
    qint64 m_lastCollapseLogTime;
    unsigned int m_lastAchievedBitrateKbps;
};

#endif // SRTADAPTIVEBITRATESTREAMING_H
