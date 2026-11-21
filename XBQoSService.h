#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>
#include <QElapsedTimer>
#include <QStringList>
#include "VideoResolutionAdapter.h"
#include "C2BitrateGuard.h"
#include "ABRConfigs.h"   // → ABRTuning.h (ABR_HEADROOM_FACTOR_PCT headroom default)

class ABRFactory;
class NetworkHandler;
class RouterNetworkHandler;
class SiyiLinkHandler;
class CameraControl;
class QoSDebugSender;
class QTimer;
class QNetworkReply;

class XBQoSService : public QObject
{
    Q_OBJECT
public:
    enum class ProtocolMode { Srt, Tcp };

    explicit XBQoSService(QObject *parent = nullptr);
    virtual ~XBQoSService();

    // Declared BEFORE start(): the per-interface ABR instances are created from
    // this list, and `isC2Configured` is the global "is C2 deployed at all"
    // flag every instance needs (report §6b). Adding/removing an interface is
    // one line here — see main.cpp.
    void setRouterAbrInterfaces(const QStringList &ifaces, bool isC2Configured);
    void setProtocolMode(ProtocolMode mode);

    // Phase 09 (headroom): the steady-state cap percent for the router ABR
    // (--abr-headroom). Declared before start(); forwarded to ABRFactory, which
    // applies it per instance at creation. Router ABR only.
    void setAbrHeadroomPct(int pct);

    // Toggle camera control while continuing to process telemetry.
    void setAbrDisabled(bool disabled);

    // QoS debug channel. Default OFF. When on, taps the SAME router rows the
    // ABR reads and publishes them (QS format) to qos-dashboard. Never feeds
    // back into the QoS path — it only reads.
    void setQosDebug(bool enabled, const QString &host, quint16 port,
                     const QString &logDir);

    void start();
    void stop();

private:
    unsigned int configuredMaxBitrateKbps() const;
    void printStartupBanner();
    void setupConnections();
    void dispatchToCameraServer(int bitrate);

    ABRFactory *m_abrFactory;
    NetworkHandler *m_networkHandler;
    RouterNetworkHandler *m_routerNetworkHandler;
    SiyiLinkHandler *m_siyiHandler;
    CameraControl *m_cameraControl;
    QoSDebugSender *m_qosDebug;
    VideoResolutionAdapter m_resolutionAdapter;

    // QoS debug channel config, declared in main.cpp; applied in start().
    bool m_qosDebugEnabled = false;
    QString m_qosDebugHost;
    quint16 m_qosDebugPort = 0;
    QString m_qosDebugLogDir;

    QStringList m_routerAbrIfaces;
    ProtocolMode m_protocolMode = ProtocolMode::Srt;
    bool m_isC2Configured = false;
    // Phase 09 (headroom): steady-state cap percent, from --abr-headroom.
    int m_abrHeadroomPct = ABR_HEADROOM_FACTOR_PCT;

    bool m_abrDisabled = false;
    bool m_awaitingAbrInput = false;

    unsigned int m_currentBitrate;
    bool m_cameraNeedsSync;
    bool m_c2ResumePending = false;
    QTimer *m_cameraKeepAliveTimer;
    QTimer *m_c2GuardTimer = nullptr;
    QNetworkReply *m_serialStatusReply = nullptr;
    bool m_serialConnected = false;
    C2BitrateGuard m_c2Guard{SRTTuning::DEFAULT_MAX_BITRATE_KBPS, SRTTuning::C2_SURVIVAL_FLOOR_KBPS};
    unsigned int m_requestedBitrate = 0;
    QElapsedTimer m_c2Clock;
    qint64 m_lastValidSiyiMs = -1;
};

#endif // XB_QOS_SERVICE_H
