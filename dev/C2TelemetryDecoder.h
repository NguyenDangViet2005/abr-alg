#ifndef C2TELEMETRYDECODER_H
#define C2TELEMETRYDECODER_H

#include "../C2TelemetryStat.h"
#include <QByteArray>
#include <QVector>
#include <QStringList>
#include <memory>

// Platform-independent decoder; timestamps must use the same monotonic clock.
class C2TelemetryDecoder {
public:
    explicit C2TelemetryDecoder(quint16 port = 3345, quint8 systemId = 1,
                                quint8 componentId = 1, qint64 staleMs = 2000,
                                const QString &fcSourceIp = QString(),
                                const QStringList &allowedFcSourceIps = QStringList());
    ~C2TelemetryDecoder();
    void ingest(const QByteArray &ethernetFrame, qint64 nowMs);
    QVector<C2TelemetryStat> snapshot(qint64 nowMs);
    void addCaptureDrops(quint64 count);
    QString diagnosticSummary() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif
