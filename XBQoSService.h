#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>
#include "VideoResolutionAdapter.h"

class ABRFactory;
class NetworkHandler;
class CameraControl;
class QTimer;

class XBQoSService : public QObject
{
    Q_OBJECT
public:
    explicit XBQoSService(QObject *parent = nullptr);
    virtual ~XBQoSService();

    void start();
    void stop();

private:
    void printStartupBanner();
    void setupConnections();
    void dispatchToCameraServer(int bitrate);

    ABRFactory *m_abrFactory;
    NetworkHandler *m_networkHandler;
    CameraControl *m_cameraControl;
    VideoResolutionAdapter m_resolutionAdapter;

    unsigned int m_currentBitrate;
    bool m_cameraNeedsSync;
    QTimer *m_cameraKeepAliveTimer;
};

#endif // XB_QOS_SERVICE_H