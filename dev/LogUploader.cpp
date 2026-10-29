#include "LogUploader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QDebug>

// ── Upload endpoint ──────────────────────────────────────────────────────────
// The log server exposes POST /api/logs/<name>: raw body (application/octet-
// stream, NOT multipart), <name> = the file's basename. It assigns the real
// stored name (a collision becomes "<name>-1"), returned as {"saved":"<name>"}.
// Its own limits/errors are the server's concern: 400 on a bad name/magic/empty
// body, 413 when the body exceeds 200MB. We only need to send the right URL and
// content type; a non-2xx reply is reported as a failure.
static const QString UPLOAD_PATH_PREFIX = QStringLiteral("/api/logs/");
static const QString UPLOAD_CONTENT_TYPE = QStringLiteral("application/octet-stream");

// Generous: a 200MB body over a slow link must not hit the timeout. The server
// caps at 200MB (413); we guard BEFORE reading so a big file fails fast with a
// clear message instead of after a 200MB transfer.
static constexpr qint64 UPLOAD_MAX_BYTES = 200LL * 1024 * 1024;
static constexpr int UPLOAD_TRANSFER_TIMEOUT_MS = 120000;

// Matches the files QoSDebugSender::startFileLog() writes
// (`qos-session-YYYYMMDD-HHMMSS.qsdb`). Kept in sync by convention, not by
// include: the sender owns the writer, this owns the reader.
static const QString LOG_FILE_GLOB = QStringLiteral("qos-session-*.qsdb");

LogUploader::LogUploader(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

LogUploader::~LogUploader() = default;

QString LogUploader::findLatestLog(const QString &dir)
{
    if (dir.isEmpty()) {
        return QString();
    }
    QDir d(dir);
    if (!d.exists()) {
        return QString();
    }

    const QFileInfoList files = d.entryInfoList({ LOG_FILE_GLOB }, QDir::Files, QDir::Time);
    // QDir::Time sorts newest-first, so entry 0 is the latest — but sort
    // explicitly on lastModified to be independent of QDir's tie-breaking.
    QFileInfo latest;
    for (const QFileInfo &fi : files) {
        if (!latest.exists() || fi.lastModified() > latest.lastModified()) {
            latest = fi;
        }
    }
    return latest.exists() ? latest.absoluteFilePath() : QString();
}

void LogUploader::uploadLatest(const QString &dir, const QString &hostPort)
{
    const int colon = hostPort.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == hostPort.size() - 1) {
        finish(false, QStringLiteral("invalid --upload value '%1' (expected <host>:<port>)").arg(hostPort));
        return;
    }
    const QString host = hostPort.left(colon);
    bool portOk = false;
    const uint port = hostPort.mid(colon + 1).toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) {
        finish(false, QStringLiteral("invalid port in --upload value '%1'").arg(hostPort));
        return;
    }

    const QString filePath = findLatestLog(dir);
    if (filePath.isEmpty()) {
        finish(false, QStringLiteral("no %1 found in '%2'").arg(LOG_FILE_GLOB, dir));
        return;
    }

    qInfo().noquote() << QString("[LogUpload] latest log: %1").arg(filePath);
    doUpload(filePath, host, static_cast<quint16>(port));
}

void LogUploader::finish(bool ok, const QString &message)
{
    // ALWAYS emit via the event loop, never synchronously. Every validation
    // path above can run BEFORE main.cpp reaches QCoreApplication::exec(), and
    // QCoreApplication::exit() called before exec() is dropped — the loop then
    // blocks forever and the process hangs. Deferring guarantees the receiver
    // (which calls exit()) runs only once exec() is pumping events.
    QMetaObject::invokeMethod(this, [this, ok, message]() {
        emit finished(ok, message);
    }, Qt::QueuedConnection);
}

void LogUploader::doUpload(const QString &filePath, const QString &host, quint16 port)
{
    // STREAM the file, never readAll(): post(request, QIODevice*) has Qt read it
    // off disk as the socket drains. readAll() would hold the whole body in RAM
    // AND Qt copies it internally AND the finished lambda would capture another
    // copy — ~3x the file resident at once. At the 200MB cap that is a real OOM
    // risk on a router-class box. The reply owns the device (setParent below), so
    // it lives exactly until the transfer ends.
    QFile *file = new QFile(filePath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        finish(false, QStringLiteral("cannot open '%1': %2").arg(filePath, file->errorString()));
        delete file;
        return;
    }
    // Fail fast on the server's 200MB cap — streaming it just to have the server
    // 413 it would waste the bandwidth and the wait.
    const qint64 bodySize = file->size();
    if (bodySize > UPLOAD_MAX_BYTES) {
        finish(false, QStringLiteral("log '%1' is %2 MB, over the %3 MB server limit")
                          .arg(filePath).arg(bodySize / (1024 * 1024))
                          .arg(UPLOAD_MAX_BYTES / (1024 * 1024)));
        delete file;
        return;
    }

    // <name> = basename, percent-encoded (a raw space or '#' in a name would
    // otherwise corrupt the path). The server rejects names containing '/' or
    // '../' itself; we never send any, since QFileInfo::fileName() has neither.
    const QString name = QFileInfo(filePath).fileName();
    const QString apiPath = UPLOAD_PATH_PREFIX + QString::fromUtf8(QUrl::toPercentEncoding(name));
    const QUrl url(QStringLiteral("http://%1:%2%3").arg(host).arg(port).arg(apiPath));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, UPLOAD_CONTENT_TYPE);
    // INACTIVITY timeout, not a total-transfer budget: it aborts only when the
    // connection stalls this long mid-body, so a large file over a slow-but-live
    // link still completes. Generous value covers a stalled peer.
    request.setTransferTimeout(UPLOAD_TRANSFER_TIMEOUT_MS);

    qInfo().noquote() << QString("[LogUpload] -> POST %1 (%2 bytes)").arg(url.toString()).arg(bodySize);

    m_reply = m_nam->post(request, file);
    file->setParent(m_reply);   // reply frees the device when it is destroyed
    QObject::connect(m_reply, &QNetworkReply::finished, this, [this, url, bodySize]() {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError err = reply->error();
        const QString errStr = reply->errorString();
        const QByteArray resp = reply->readAll();
        reply->deleteLater();

        // Success is 2xx ONLY. A 3xx (no redirect policy is set, and POST is not
        // auto-followed) or any other non-2xx must NOT be parsed as {"saved"}.
        if (err != QNetworkReply::NoError || status < 200 || status >= 300) {
            // Surface the server's own message when it sent one — its 400 body
            // is {"error":"..."} ("Tên file không hợp lệ", "File rỗng", …).
            const QJsonObject obj = QJsonDocument::fromJson(resp).object();
            const QString serverMsg = obj.value(QStringLiteral("error")).toString();
            finish(false, QStringLiteral("upload failed (%1): %2 (HTTP %3)%4")
                              .arg(url.toString(), errStr)
                              .arg(status)
                              .arg(serverMsg.isEmpty() ? QString() : QStringLiteral(" — ") + serverMsg));
            return;
        }
        // 200 body is {"saved":"<real-name>"} — report it, since a collision
        // makes the stored name differ from the one we sent.
        const QJsonObject obj = QJsonDocument::fromJson(resp).object();
        const QString saved = obj.value(QStringLiteral("saved")).toString();
        finish(true, QStringLiteral("uploaded %1 bytes to %2 (HTTP %3)%4")
                         .arg(bodySize)
                         .arg(url.toString())
                         .arg(status)
                         .arg(saved.isEmpty() ? QString()
                                              : QStringLiteral(" — saved as '%1'").arg(saved)));
    });
}
