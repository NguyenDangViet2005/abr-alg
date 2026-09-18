#ifndef CAMERACONTROL_H
#define CAMERACONTROL_H

#include <QObject>
#include <QNetworkAccessManager>

class CameraControl : public QObject
{
    Q_OBJECT
public:
    explicit CameraControl(QObject *parent = nullptr);

    // Gửi yêu cầu thay đổi bitrate tới camera API (POST JSON: { "bitrate": <kbps> })
    void sendAdaptBitrate(int bitrate);

public slots:
    void handleChangeCameraBitrate(int bitrate);

private:
    QNetworkAccessManager* m_nam;
};

#endif // CAMERACONTROL_H
