#include "CameraControl.h"

#ifndef XBFIRM
#include "ABRConfigs.h"
#endif

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QDebug>

// Endpoint camera adapt-bitrate (HTTP POST JSON: { "bitrate": <kbps> }).
// Build từ ABRConfigs.h: host + port + path.
static const QString CAMERA_ADAPT_BITRATE_URL = QString("http://%1:%2%3")
    .arg(CAMERA_ADAPT_BITRATE_HOST)
    .arg(CAMERA_ADAPT_BITRATE_PORT)
    .arg(CAMERA_ADAPT_BITRATE_PATH);

CameraControl::CameraControl(QObject *parent)
    : QObject{parent}
    , m_nam(new QNetworkAccessManager(this))
{}

void CameraControl::sendAdaptBitrate(int bitrate)
{
    QUrl url(CAMERA_ADAPT_BITRATE_URL);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject payload;
    payload["bitrate"] = bitrate;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    qInfo().noquote() << QString("[CameraControl] POST %1: %2")
                             .arg(CAMERA_ADAPT_BITRATE_URL)
                             .arg(QString::fromUtf8(body));

    QNetworkReply* reply = m_nam->post(request, body);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, url, bitrate]() {
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << QString("[CameraControl] adapt-bitrate FAILED (%1): %2")
                                       .arg(url.toString())
                                       .arg(reply->errorString());
        } else {
            const QByteArray respData = reply->readAll();
            qInfo().noquote() << QString("[CameraControl] Response from camera server: %1")
                                       .arg(QString::fromUtf8(respData));

            QJsonDocument doc = QJsonDocument::fromJson(respData);
            bool isSuccess = true;
            QString message;
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                if (obj.contains("success")) {
                    isSuccess = obj["success"].toBool();
                }
                if (obj.contains("message")) {
                    message = obj["message"].toString();
                }
            }

            if (!isSuccess) {
                qWarning().noquote() << QString(">>> [CameraControl] \033[1;31mREJECTED\033[0m: Camera rejected [%1 kbps] -> Reason: %2 <<<")
                                           .arg(bitrate)
                                           .arg(message.isEmpty() ? QString::fromUtf8(respData) : message);
            } else {
                qInfo().noquote() << QString(">>> [CameraControl] \033[1;32mSUCCESS\033[0m: Camera adjusted to \033[1;33m[%1 kbps]\033[0m <<<")
                                           .arg(bitrate);
            }
        }
        reply->deleteLater();
    });
}

void CameraControl::handleChangeCameraBitrate(int bitrate)
{
    sendAdaptBitrate(bitrate);
}
