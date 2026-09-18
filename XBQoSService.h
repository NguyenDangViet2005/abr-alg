#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>
#include "VideoResolutionAdapter.h"

class ABRFactory;
class AICompressor;
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
    AICompressor *m_aiCompressor;
    NetworkHandler *m_networkHandler;
    CameraControl *m_cameraControl;
    VideoResolutionAdapter m_resolutionAdapter;
    bool m_isVideoStreamEnabled;
    unsigned int m_currentBitrate;
    QTimer *m_cameraKeepAliveTimer;
};

#endif // XB_QOS_SERVICE_H