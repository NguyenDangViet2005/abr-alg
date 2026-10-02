#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>
#include <QStringList>
#include "VideoResolutionAdapter.h"

class ABRFactory;
class NetworkHandler;
class RouterNetworkHandler;
class CameraControl;
class QoSDebugSender;
class QTimer;

class XBQoSService : public QObject
{
    Q_OBJECT
public:
    explicit XBQoSService(QObject *parent = nullptr);
    virtual ~XBQoSService();

    // Declared BEFORE start(): the per-interface ABR instances are created from
    // this list, and `isC2Configured` is the global "is C2 deployed at all"
    // flag every instance needs (report §6b). Adding/removing an interface is
    // one line here — see main.cpp.
    void setRouterAbrInterfaces(const QStringList &ifaces, bool isC2Configured);

    // QoS debug channel. Default OFF. When on, taps the SAME router rows the
    // ABR reads and publishes them (QS format) to qos-dashboard. Never feeds
    // back into the QoS path — it only reads.
    void setQosDebug(bool enabled, const QString &host, quint16 port,
                     const QString &logDir);

    void start();
    void stop();

private:
    void printStartupBanner();
    void setupConnections();
    void dispatchToCameraServer(int bitrate);

    ABRFactory *m_abrFactory;
    NetworkHandler *m_networkHandler;
    RouterNetworkHandler *m_routerNetworkHandler;
    CameraControl *m_cameraControl;
    QoSDebugSender *m_qosDebug;
    VideoResolutionAdapter m_resolutionAdapter;

    // QoS debug channel config, declared in main.cpp; applied in start().
    bool m_qosDebugEnabled = false;
    QString m_qosDebugHost;
    quint16 m_qosDebugPort = 0;
    QString m_qosDebugLogDir;

    QStringList m_routerAbrIfaces;
    bool m_isC2Configured = false;

    unsigned int m_currentBitrate;
    bool m_cameraNeedsSync;
    QTimer *m_cameraKeepAliveTimer;
};

#endif // XB_QOS_SERVICE_H