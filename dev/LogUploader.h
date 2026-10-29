#ifndef LOGUPLOADER_H
#define LOGUPLOADER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// One-shot log uploader for the `--upload <host>:<port>` CLI mode.
//
// The service is normally a long-running daemon, but `--upload` turns it into a
// single HTTP POST: find the newest `.qsdb` session file in the log dir, send it
// raw to the given host:port, then exit. No QoS engine, no UDP listeners, no
// sockets — main.cpp returns right after this completes.
//
// WHY A SEPARATE CLASS: the upload is an HTTP client with its own error paths
// (missing file, connection refused, non-2xx). Keeping it out of XBQoSService
// means the daemon path is untouched — the two modes share nothing but the log
// directory constant.
class LogUploader : public QObject
{
    Q_OBJECT
public:
    explicit LogUploader(QObject *parent = nullptr);
    ~LogUploader() override;

    // Newest `qos-session-*.qsdb` in `dir` by modification time, as an absolute
    // path. Empty when the dir does not exist or holds no matching file.
    // Static + side-effect-free so it can be unit-tested without any network.
    static QString findLatestLog(const QString &dir);

    // POST the newest log in `dir` to http://<host>:<port>/api/logs/<basename>
    // as a raw body (application/octet-stream). `hostPort` is the raw
    // "<host>:<port>" CLI value; `ok` is false on any parse/IO/HTTP failure.
    // Emits finished() exactly once, success or failure.
    void uploadLatest(const QString &dir, const QString &hostPort);

signals:
    // Exactly one of these fires per uploadLatest() call.
    void finished(bool ok, const QString &message);

private:
    void doUpload(const QString &filePath, const QString &host, quint16 port);

    // Single exit point for every path. Emits finished() via a QUEUED
    // invocation, never synchronously: uploadLatest() runs before main.cpp
    // calls exec(), and a signal emitted then would call QCoreApplication::
    // exit() too early to be honoured — the loop would never return. See .cpp.
    void finish(bool ok, const QString &message);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
};

#endif // LOGUPLOADER_H
