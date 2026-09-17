#ifndef CAMERACONTROL_H
#define CAMERACONTROL_H

#include <QObject>
#include <QNetworkAccessManager>
#include "VideoResolutionAdapter.h"

class CameraControl : public QObject
{
    Q_OBJECT
public:
    explicit CameraControl(QObject *parent = nullptr);

    // Gửi yêu cầu thay đổi bitrate và độ phân giải tới camera API (POST JSON: { "bitrate": <kbps>, ... })
    void sendAdaptBitrate(int bitrate, const VideoProfile &profile = VideoProfile{0, 0, 0, 0, ""});

public slots:
    void handleChangeCameraBitrate(int bitrate);

private:
    QNetworkAccessManager* m_nam;
};

#endif // CAMERACONTROL_H
