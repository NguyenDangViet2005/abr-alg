#ifndef CAMERACONTROL_H
#define CAMERACONTROL_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>

class CameraControl : public QObject
{
    Q_OBJECT
public:
    explicit CameraControl(QObject *parent = nullptr);

    void sendAdaptBitrate(int bitrate);

    void requestStreamRefresh(int currentBitrateKbps);

signals:
    void bitrateReported(int requestedKbps, int achievedKbps);
    void bitrateRejected(int requestedKbps, const QString &reason);

public slots:
    void handleChangeCameraBitrate(int bitrate);

private:
    QNetworkAccessManager* m_nam;
};

#endif // CAMERACONTROL_H
