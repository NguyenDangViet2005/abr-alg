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

    qInfo().noquote() << QString("[CameraControl -> HTTP API] POST %1: %2")
                             .arg(CAMERA_ADAPT_BITRATE_URL)
                             .arg(QString::fromUtf8(body));

    QNetworkReply* reply = m_nam->post(request, body);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, url]() {
        if (reply->error() == QNetworkReply::NoError) {
            qInfo().noquote() << QString("[CameraControl] adapt-bitrate OK (%1): %2")
                                     .arg(url.toString())
                                     .arg(QString::fromUtf8(reply->readAll()));
        } else {
            qWarning().noquote() << QString("[CameraControl] adapt-bitrate FAILED (%1): %2")
                                       .arg(url.toString())
                                       .arg(reply->errorString());
        }
        reply->deleteLater();
    });
}

void CameraControl::handleChangeCameraBitrate(int bitrate)
{
    sendAdaptBitrate(bitrate);
}
