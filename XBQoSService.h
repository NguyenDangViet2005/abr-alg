#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>
#include "VideoResolutionAdapter.h"

class ABRFactory;
class AICompressor;
class NetworkHandler;
class QProcess;
class QUdpSocket;

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
    void startCameraStreamer();
    void stopCameraStreamer();
    void sendCameraControlCommand(int bitrate, const VideoProfile &profile, bool enabled);

    ABRFactory *m_abrFactory;
    AICompressor *m_aiCompressor;
    NetworkHandler *m_networkHandler;
    QProcess *m_cameraProcess;
    QUdpSocket *m_camControlSocket;
    VideoResolutionAdapter m_resolutionAdapter;
    bool m_isVideoStreamEnabled;
};

#endif // XB_QOS_SERVICE_H