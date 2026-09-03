#ifndef SRTADAPTIVEBITRATESTREAMING_H
#define SRTADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QDebug>
#include <QVariantList>
#include <QDateTime>
#include <QtMath>
#include "IAdaptiveBitrateStreaming.h"

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    // ── BelaCoder Configuration Constants ──
    static constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS          = 300;
    static constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS          = 6000;
    static constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS      = 2000;

    // Bitrate adjustment step scales (belacoder.c)
    static constexpr unsigned int BITRATE_INCR_MIN_KBPS             = 30;   // the minimum bitrate increment step (30 kbps)
    static constexpr unsigned int BITRATE_INCR_SCALE                = 30;   // bitrate += BITRATE_INCR_MIN + bitrate/BITRATE_INCR_SCALE
    static constexpr unsigned int BITRATE_DECR_MIN_KBPS             = 100;  // the minimum value to decrease bitrate by (100 kbps)
    static constexpr unsigned int BITRATE_DECR_SCALE                = 10;   // bitrate -= BITRATE_DECR_MIN + bitrate/BITRATE_DECR_SCALE

    // Cooldown intervals (ms) from belacoder.c
    static constexpr qint64 BITRATE_INCR_INT_MS                     = 500;  // (clear) min interval for increasing bitrate
    static constexpr qint64 BITRATE_DECR_INT_MS                     = 200;  // (light congestion) min interval for decreasing bitrate
    static constexpr qint64 BITRATE_DECR_FAST_INT_MS                = 250;  // (heavy congestion) min interval for decreasing bitrate

    // Latency and Rounding
    static constexpr int DEFAULT_SRT_LATENCY_MS                     = 2000; // Standard negotiated SRT buffer latency (ms)
    static constexpr unsigned int BITRATE_ROUNDING_STEP_KBPS        = 50;   // Làm tròn theo nấc 50 kbps

    enum class CongestionState {
        Clear = 0,
        Light,
        Heavy,
        Panic
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
    bool isRunning() const { return m_isRunning; }

public slots:
    void handleSerialStatus(bool isConnected) override;
    void handleSetMaxAbrBitrate(int maxBitrate) override;
    void handleQosCameraConnection(const QVariantList &clients);
    void handleQosControllingConnection(const QVariantList &clients);

private:
    void processSrtQos(double rtt, double bandwidthMbps, double sendRateMbps, int lossTotal, int bufferSize);
    void applyNewBitrate(unsigned int targetBitrateKbps, double rtt, double bandwidthMbps, int deltaLoss);

    bool m_isRunning;
    bool m_isConnected;

    unsigned int m_currentBitrateKbps;
    unsigned int m_minBitrateKbps;
    unsigned int m_maxBitrateKbps;
    int m_srtLatencyMs;

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
    qint64 m_nextBitrateIncr;
    qint64 m_nextBitrateDecr;

    int m_lastLossTotal;
    bool m_hasLastLoss;
    CongestionState m_lastCongestionState;
};

#endif // SRTADAPTIVEBITRATESTREAMING_H
