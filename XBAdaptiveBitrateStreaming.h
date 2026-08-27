#ifndef XBADAPTIVEBITRATESTREAMING_H
#define XBADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QtMath>
#include <QUdpSocket>
#ifdef XBFIRM
#include "GeneralConfigs.h"
#include "Settings.h"
#include "GeneralSetting.h"
#else
#include "ABRConfigs.h"
#endif
#include "IAdaptiveBitrateStreaming.h"


struct tcpInfo {
    quint32 rtt;                 // Round-trip time (milliseconds)
    quint32 min_rtt;             // Minimum RTT (milliseconds)
    double throughput;           // Current delivery rate (kbps)
    double loss_rate;            // Packet loss rate (percentage)
    quint32 siocoutq;            // Send queue size (bytes)
    quint32 so_sndbuf;           // Send buffer size (bytes)
};

struct AbrState { unsigned int current_bitrate_kbps; };
enum LastAction { ABR_NONE, ABR_INCREASE, ABR_DECREASE };

// 🎨 UX Status for End Users - Reflects streaming quality experience
enum StreamingQualityStatus {
    PLAYING_AUTO = 0,        // ABR ổn định, chất lượng phù hợp với mạng, trải nghiệm xem mượt
    OPTIMIZING = 1,          // ABR đang điều chỉnh chất lượng do mạng dao động
    POOR_NETWORK = 2,        // Mạng yếu, ABR hạ chất lượng xuống thấp
    BUFFERING = 3,           // Mạng quá kém, video bị đứng hình, đang chờ dữ liệu
    LOADING = 4              // ABR recovery/re-evaluation sau gián đoạn
};

class XBAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:

    // Bitrate constraints
    const unsigned int  MIN_BITRATE_KBPS  = 30;
    const unsigned int  MAX_BITRATE_KBPS  = 2000;
    const unsigned int  BITRATE_STEP_KBPS = 100;
    const unsigned int  MIN_BITRATE_CHANGE_THRESHOLD = 30;

    // Algorithm parameters
    const size_t        RTT_HISTORY_SIZE  = 10;
    const quint32       RTT_HIGH_THRESHOLD_MS = 500;
    float               QUEUE_DELAY_HIGH_THRESHOLD_MS = 400.0;
    float               QUEUE_DELAY_LOW_THRESHOLD_MS = 100.0;
    float               QUEUE_DELAY_LOW_AFTER_DECREASE_MS = 10.0;
    float               RTT_STABLE_THRESHOLD_MS = 50.0;
    float               RTT_STABLE_AFTER_DECREASE_MS = 15.0;
    float               JITTER_STABLE_THRESHOLD_MS = 15.0;
    float               TELEMETRY_STABLE_TIMEOUT_MS = 3000.0;
    unsigned int        TELEMETRY_KICK_BITRATE_KBPS = 500;
    qint64              BITRATE_INCREASE_COOLDOWN_MS = 2000;

    explicit XBAdaptiveBitrateStreaming(QObject *parent = nullptr);
    ~XBAdaptiveBitrateStreaming();

    static XBAdaptiveBitrateStreaming* instance();
    unsigned int getCurrentBitrate() const { return m_abrState.current_bitrate_kbps; }

    unsigned int maxAbrBitrate() const;
    void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) override;

    void start();
    void stop();
    void reset(int bitrateKbps);
public slots:
    void handleSerialStatus(bool isConnected);
    void handleSetMaxAbrBitrate(int maxBitrate);
    void handleChangeBitrateStep(int bitrateStep);
    void processTCPInfo(const tcpInfo &tcpi);
    void processTCPInfo(quint32 rtt,
                        quint32 min_rtt,
                        double throughput,
                        double loss_rate,
                        quint32 siocoutq,
                        quint32 so_sndbuf,
                        int tcpiSegsout);
    void processTCPInfo(double rtt,
                        double deliveryRate,
                        double tcpiRtt,
                        int tcpiLoss,
                        int tcpiSegsout,
                        int tcpiSndCwnd,
                        int tcpiSndMss,
                        int tcpiLastAckRecv,
                        int sockOutq,
                        int sockSndBuf,
                        int tcpiMinRtt);
    void processTelemetryTCPInfo(double rtt,
                                 double deliveryRate,
                                 double tcpiRtt,
                                 int tcpiLoss,
                                 int tcpiSegsout,
                                 int tcpiSndCwnd,
                                 int tcpiSndMss,
                                 int tcpiLastAckRecv,
                                 int sockOutq,
                                 int sockSndBuf,
                                 int tcpiMinRtt);
    void updateAlphaValues(double alpha, double gainUp, double gainDown);
private:
    bool _cameraStatus;

    //======= Member variables for CORE
    AbrState m_abrState;
    QVector<quint32> m_rttHistory;
    LastAction m_lastAbrAction;
    unsigned int m_problematicBitrateThreshold;
    qint64 m_lastQualityChangeTime;
    double m_avgSiocoutq;

    unsigned int _maxAbrBitrate;
    static XBAdaptiveBitrateStreaming* _instance;

    // Variable for Advisor Module
    double m_alpha;    // Reliability Coefficient for ABR CORE
    double m_gainUp;   // Using for change bitrate step up
    double m_gainDown; // Using for change bitrate step down

    // UX Status tracking
    StreamingQualityStatus m_currentStatus;
    StreamingQualityStatus m_previousStatus;
    int m_consecutivePoorConditions;  // Đếm số lần liên tiếp gặp điều kiện kém
    qint64 m_lastStatusChangeTime;

    // Network data monitoring
    QTimer* m_networkDataTimer;
    qint64 m_lastNetworkDataTime;
    bool m_isStarted;

    // Cumulative tracking for delta calculation
    int _lastTcpiSegsout;

    int _lastTelemetrySegsout;

    // Telemetry data
    quint32 m_telemetryRtt;
    quint32 m_telemetryMinRtt;
    double m_telemetryLossRate;

    // Periodic status broadcast
    QTimer* m_statusBroadcastTimer;
    qint64 m_telemetryStableSince;

    // ======= Core Helper Functions
    bool haveLostPackets(const tcpInfo &tcpi);
    bool rttHigh(const tcpInfo &tcpi);
    double calculateJitter();
    void checkSenderQueue(const tcpInfo &tcpi);

    void setNewBitrate(unsigned int new_bitrate_kbps);
    void updateStreamingStatus(const tcpInfo &tcpi, bool bitrateChanged);
    void onNetworkDataTimeout();
    void onStatusBroadcastTimeout();

    QUdpSocket _udpSocket;
    bool _haveTelemetry;
    int _bitrateStepKps;
};

#endif // XBADAPTIVEBITRATESTREAMING_H
