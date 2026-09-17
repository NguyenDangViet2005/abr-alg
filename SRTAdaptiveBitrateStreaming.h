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

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    // ── BelaCoder Configuration Constants ──
    static constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS          = 0;
    static constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS          = 6000;
    static constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS      = 1000;
    static constexpr unsigned int MIN_ACTIVE_VIDEO_BITRATE_KBPS     = 300; // Nấc sàn tối thiểu của video đang chạy (360p Low)

    static constexpr unsigned int BITRATE_INCR_MIN_KBPS             = 50;
    static constexpr unsigned int BITRATE_INCR_MAX_STEP_KBPS        = 200;
    static constexpr unsigned int BITRATE_DECR_MIN_KBPS             = 100;

    static constexpr qint64 BITRATE_INCR_DECISION_INTERVAL_MS       = 1000;
    static constexpr qint64 BITRATE_DECR_FAST_INTERVAL_MS           = 250;
    static constexpr qint64 BITRATE_DECR_NORMAL_INTERVAL_MS         = 400;
    static constexpr qint64 RECOVERY_COOLDOWN_MS                    = 2000;

    static constexpr int CONSECUTIVE_CLEAR_REQUIRED                 = 4;
    static constexpr int SLIDING_WINDOW_SIZE                        = 5;
    static constexpr double MIN_VALID_RTT_MS                        = 5.0;

    // Latency and Rounding
    static constexpr int DEFAULT_SRT_LATENCY_MS                     = 2000; // Standard negotiated SRT buffer latency (ms)
    static constexpr unsigned int BITRATE_ROUNDING_STEP_KBPS        = 10;   // Làm tròn nấc mịn 10 kbps (thay vì 50 kbps)

    enum class CongestionState {
        Clear = 0,
        Light,
        HeavyLight,
        HeavyModerate,
        HeavySevere,
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
    void setSrtLatency(int latencyMs);

    unsigned int currentBitrate() const { return m_currentBitrateKbps; }
    CongestionState congestionState() const { return m_lastCongestionState; }
    C2Quality c2Quality() const { return m_c2Quality; }
    C2PriorityLevel c2Priority() const { return m_c2Priority; }
    bool isVideoEnabled() const { return m_isVideoEnabled; }
    bool isRunning() const { return m_isRunning; }

public slots:
    void handleSerialStatus(bool isConnected) override;
    void handleSetMaxAbrBitrate(int maxBitrate) override;
    void handleQosCameraConnection(const QVector<SRTPeerStat> &peers);
    void handleQosControllingConnection(const QVector<SRTPeerStat> &peers);
    void handleC2ConnectionStats(const QVector<SRTPeerStat> &peers);
    void onHeartbeatTimeout();

private:
    void processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps, int rawLossTotal);
    void applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss);

    void evaluateC2Quality();
    QString c2QualityToString(C2Quality q) const;
    QString c2PriorityToString(C2PriorityLevel p) const;

    double calculateMedian(QVector<double> list);
    double calculateAverage(const QVector<double> &list);

    bool m_isRunning;
    bool m_isConnected;

    unsigned int m_currentBitrateKbps;
    unsigned int m_minBitrateKbps;
    unsigned int m_maxBitrateKbps;
    int m_srtLatencyMs;

    // C2 Telemetry & Priority State
    C2Quality m_c2Quality;
    C2PriorityLevel m_c2Priority;
    bool m_isVideoEnabled;
    bool m_isExplicitC2Only;
    double m_c2Rtt;
    int m_c2Retransmits;
    int m_c2Loss;
    int m_c2Unacked;
    qint64 m_lastC2PacketTime;

    // Sliding window sample histories
    QVector<double> m_rttHistory;
    QVector<double> m_bwHistory;
    QVector<int> m_lossHistory;

    double m_rttAvg;
    double m_rttAvgDelta;
    double m_prevRtt;
    double m_rttMin;
    double m_rttJitter;

    double m_throughput;
    qint64 m_lastBitrateChangeTime;
    qint64 m_lastBitrateIncrTime;
    qint64 m_cooldownUntilMs;
    int m_consecutiveClearCount;

    QTimer *m_heartbeatTimer;
    double m_latestSmoothedRtt;
    double m_latestSmoothedBw;
    int m_latestDeltaLoss;
    QString m_latestStatusReason;
    qint64 m_lastQosPacketTime;

    int m_lastLossTotal;
    bool m_hasLastLoss;
    bool m_isBootstrapped;
    CongestionState m_lastCongestionState;
};

#endif // SRTADAPTIVEBITRATESTREAMING_H
