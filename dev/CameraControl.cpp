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
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    QUrl url(CAMERA_ADAPT_BITRATE_URL);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(2000);

    QJsonObject payload;
    payload["bitrate"] = bitrate;
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    qInfo().noquote() << QString("[CameraControl] POST %1: %2")
                             .arg(CAMERA_ADAPT_BITRATE_URL)
                             .arg(QString::fromUtf8(body));

    QNetworkReply* reply = m_nam->post(request, body);
    m_currentReply = reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url, bitrate]() {
        if (m_currentReply == reply) {
            m_currentReply = nullptr;
        }

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError) {
                qWarning().noquote() << QString("[CameraControl] adapt-bitrate FAILED (%1): %2")
                                           .arg(url.toString())
                                           .arg(reply->errorString());
                emit bitrateRejected(bitrate, reply->errorString());
            }
            reply->deleteLater();
            return;
        }

        const QByteArray respData = reply->readAll();
        qInfo().noquote() << QString("[CameraControl] Response from camera server: %1")
                                   .arg(QString::fromUtf8(respData));

        QJsonDocument doc = QJsonDocument::fromJson(respData);
        bool isSuccess = true;
        QString message;
        int achievedKbps = 0;
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("success")) {
                isSuccess = obj["success"].toBool();
            }
            if (obj.contains("message")) {
                message = obj["message"].toString();
            }
            for (const char *key : {"actualKbps", "kbps", "bitrate"}) {
                if (obj.contains(key) && obj.value(key).isDouble()) {
                    achievedKbps = obj.value(key).toInt();
                    break;
                }
            }
        }

        if (!isSuccess) {
            qWarning().noquote() << QString(">>> [CameraControl] \033[1;31mREJECTED\033[0m: Camera rejected [%1 kbps] -> Reason: %2 <<<")
                                       .arg(bitrate)
                                       .arg(message.isEmpty() ? QString::fromUtf8(respData) : message);
            emit bitrateRejected(bitrate, message.isEmpty() ? QString::fromUtf8(respData) : message);
        } else {
            qInfo().noquote() << QString(">>> [CameraControl] \033[1;32mSUCCESS: Camera adjusted to [%1 kbps]\033[0m (output: %2) <<<")
                                       .arg(bitrate)
                                       .arg(achievedKbps > 0 ? QString("%1 kbps").arg(achievedKbps)
                                                             : QString("n/a"));
        }
        emit bitrateReported(bitrate, achievedKbps);
        reply->deleteLater();
    });
}

void CameraControl::requestStreamRefresh(int currentBitrateKbps)
{
    qInfo().noquote() << QString("[CameraControl] 🚀 Request stream refresh (re-apply %1 kbps) to flush decoder")
                                 .arg(currentBitrateKbps);
    sendAdaptBitrate(currentBitrateKbps);
}

void CameraControl::handleChangeCameraBitrate(int bitrate)
{
    sendAdaptBitrate(bitrate);
}
