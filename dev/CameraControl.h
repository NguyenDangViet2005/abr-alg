#ifndef CAMERACONTROL_H
#define CAMERACONTROL_H

#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QNetworkReply>
#include <QTimer>

class CameraControl : public QObject
{
    Q_OBJECT
public:
    explicit CameraControl(QObject *parent = nullptr);

    void sendAdaptBitrate(int bitrate);
    void setVideoPendingForC2(bool pending);
    void requestStreamRefresh(int currentBitrateKbps);

signals:
    void bitrateReported(int requestedKbps, int achievedKbps);
    void bitrateRejected(int requestedKbps, const QString &reason);

private:
    void syncVideoPendingForC2();

    QNetworkAccessManager* m_nam;
    QPointer<QNetworkReply> m_currentReply;
    QPointer<QNetworkReply> m_c2PendingReply;
    QTimer m_c2PendingRetry;
    bool m_videoPendingForC2 = false;
    bool m_c2PendingNeedsSync = false;
};

#endif // CAMERACONTROL_H
