#ifndef XB_QOS_SERVICE_H
#define XB_QOS_SERVICE_H

#include <QObject>

class ABRFactory;
class AICompressor;
class NetworkHandler;
class QProcess;

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

    ABRFactory *m_abrFactory;
    AICompressor *m_aiCompressor;
    NetworkHandler *m_networkHandler;
    QProcess *m_cameraProcess;
};

#endif // XB_QOS_SERVICE_H