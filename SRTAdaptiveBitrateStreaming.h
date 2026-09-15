#ifndef SRTADAPTIVEBITRATESTREAMING_H
#define SRTADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QDebug>
#include <QVariantList>
#include <QDateTime>
#include <QTimer>
#include <QVector>
#include <QtMath>
#include "IAdaptiveBitrateStreaming.h"

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    // ── BelaCoder Configuration Constants ──
    static constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS          = 0;
    static constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS          = 6000;
    static constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS      = 1000;

    // Bitrate adjustment step scales (tinh chỉnh mịn cho dải vài trăm kbps)
    static constexpr unsigned int BITRATE_INCR_MIN_KBPS             = 20;   // Tăng tối thiểu 20 kbps
    static constexpr unsigned int BITRATE_INCR_MAX_STEP_KBPS        = 100;  // Tăng tối đa 100 kbps mỗi bước
    static constexpr unsigned int BITRATE_DECR_MIN_KBPS             = 30;   // Giảm tối thiểu 30 kbps

    // Decision intervals & Cooldowns
    static constexpr qint64 BITRATE_INCR_DECISION_INTERVAL_MS       = 1000; // Decision interval for increasing bitrate (1.0s)
    static constexpr qint64 BITRATE_DECR_FAST_INTERVAL_MS           = 250;  // Fast interval for reacting to severe congestion
    static constexpr qint64 BITRATE_DECR_NORMAL_INTERVAL_MS         = 400;  // Normal interval for moderate congestion
    static constexpr qint64 RECOVERY_COOLDOWN_MS                    = 2000; // Cooldown after decrease before any increase allowed (2.0s)

    // Anti-oscillation requirements
    static constexpr int CONSECUTIVE_CLEAR_REQUIRED                 = 4;    // Need 4 consecutive CLEAR samples (~1s) to increase
    static constexpr int SLIDING_WINDOW_SIZE                        = 5;    // Moving average/median window size
    static constexpr double MIN_VALID_RTT_MS                        = 5.0;  // Physical lower bound for valid wireless RTT

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

    // Getter methods
    unsigned int currentBitrate() const { return m_currentBitrateKbps; }
    CongestionState congestionState() const { return m_lastCongestionState; }
    C2Quality c2Quality() const { return m_c2Quality; }
    C2PriorityLevel c2Priority() const { return m_c2Priority; }
    bool isVideoEnabled() const { return m_isVideoEnabled; }
    bool isRunning() const { return m_isRunning; }

public slots:
    void handleSerialStatus(bool isConnected) override;
    void handleSetMaxAbrBitrate(int maxBitrate) override;
    void handleQosCameraConnection(const QVariantList &clients);
    void handleQosControllingConnection(const QVariantList &clients);
    void handleC2ConnectionStats(const QVariantMap &c2Stats);
    void onHeartbeatTimeout();

private:
    void processSrtQos(double rawRtt, double rawBandwidthMbps, double rawSendRateMbps, int rawLossTotal, int rawBufferSize);
    void applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss);
    void evaluateC2Quality();
    QString c2QualityToString(C2Quality q) const;
    QString c2PriorityToString(C2PriorityLevel p) const;

    // Smoothing helpers
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
    double m_c2RttVar;
    int m_c2Retransmits;
    int m_c2Unacked;
    int m_c2Loss;
    qint64 m_lastC2PacketTime;

    // Sliding window sample histories
    QVector<double> m_rttHistory;
    QVector<double> m_bwHistory;
    QVector<int> m_lossHistory;

    // BelaCoder EMA & dynamic threshold metrics
    double m_rttAvg;
    double m_rttAvgDelta;
    double m_prevRtt;
    double m_rttMin;
    double m_rttJitter;

    double m_bsAvg;
    double m_bsJitter;
    int m_prevBs;

    double m_throughput;
    qint64 m_lastBitrateChangeTime;
    qint64 m_lastBitrateIncrTime;
    qint64 m_cooldownUntilMs;
    int m_consecutiveClearCount;

    // Heartbeat reporting timer
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
