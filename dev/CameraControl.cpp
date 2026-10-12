#include "CameraControl.h"
#include "ABRConfigs.h"

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

// xbfirm video-socket restart endpoint (HTTP POST, no body). Drops + reopens the
// camera's video WebSocket, which sends an RST and clears the TCP retransmit
// queue — the only userspace cure for retransmit head-of-line blocking.
static const QString CAMERA_VIDEO_RESTART_URL = QString("http://%1:%2%3")
    .arg(CAMERA_VIDEO_RESTART_HOST)
    .arg(CAMERA_VIDEO_RESTART_PORT)
    .arg(CAMERA_VIDEO_RESTART_PATH);

CameraControl::CameraControl(QObject *parent)
    : QObject{parent}
    , m_nam(new QNetworkAccessManager(this))
{}

void CameraControl::sendAdaptBitrate(int bitrate)
{
    // A new command SUPERSEDES any in-flight one: the camera would otherwise
    // receive both, and the older reply would arrive after the newer ask.
    bool superseded = false;
    if (m_currentReply && m_currentReply->isRunning()) {
        superseded = true;
        m_currentReply->disconnect();
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

    qDebug().noquote() << QString("[CameraControl] -> POST %1 %2%3")
                              .arg(url.toString())
                              .arg(QString::fromUtf8(body))
                              .arg(superseded ? QStringLiteral("  [superseded an in-flight command]") : QString());

    QNetworkReply* reply = m_nam->post(request, body);
    m_currentReply = reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url, bitrate]() {
        if (m_currentReply == reply) {
            m_currentReply = nullptr;
        }

        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError) {
                qWarning().noquote() << QString("[CameraControl] ❌ adapt-bitrate FAILED (%1): %2 (HTTP %3)")
                                           .arg(url.toString())
                                           .arg(reply->errorString())
                                           .arg(statusCode);
                emit bitrateRejected(bitrate, reply->errorString());
            }
            reply->deleteLater();
            return;
        }
        const QByteArray respData = reply->readAll();
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
        }
        qDebug().noquote() << QString("[CameraControl] <- HTTP %1  asked=%2 kbps  achieved=%3 kbps")
                                  .arg(statusCode)
                                  .arg(bitrate)
                                  .arg(achievedKbps);
        emit bitrateReported(bitrate, achievedKbps);
        reply->deleteLater();
    });
}

void CameraControl::requestStreamRefresh(int currentBitrateKbps)
{
    // Phase 12 (PORT DEVIATION): a stream "refresh" is now a HARD TCP socket
    // restart, not an adapt-bitrate re-apply. Restarting the socket aborts the old
    // connection (RST) and discards its retransmit queue — the only userspace fix
    // for TCP retransmit HOL blocking that starves realtime video. The bitrate is
    // NOT changed here; the ladder owns the bitrate decision.
    Q_UNUSED(currentBitrateKbps);

    // Supersede any in-flight restart (same rationale as sendAdaptBitrate: the
    // camera would otherwise act on both, older reply landing last).
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->disconnect();
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    const QString url = CAMERA_VIDEO_RESTART_URL;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(2000);

    qWarning().noquote() << QString("[CameraControl] -> POST %1 (video socket restart)").arg(url);

    QNetworkReply *reply = m_nam->post(request, QByteArray());   // empty body
    m_currentReply = reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        if (m_currentReply == reply) m_currentReply = nullptr;
        if (reply->error() != QNetworkReply::NoError
            && reply->error() != QNetworkReply::OperationCanceledError) {
            qWarning().noquote() << QString("[CameraControl] video restart FAILED (%1): %2")
                                        .arg(url).arg(reply->errorString());
        } else {
            qInfo().noquote() << QString("[CameraControl] <- video socket restart OK (%1)").arg(url);
        }
        reply->deleteLater();
    });
}
