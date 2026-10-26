#ifndef XBTELEMETRYHANDLER_H
#define XBTELEMETRYHANDLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#include "../C2TelemetryStat.h"
#include "../ABRConfigs.h"
#include "C2TelemetryDecoder.h"

class QSocketNotifier;

class XbTelemetryHandler : public QObject {
    Q_OBJECT
public:
    explicit XbTelemetryHandler(QObject *parent = nullptr);
    ~XbTelemetryHandler();
    bool start(const QString &interface = C2_TELEMETRY_INTERFACE, quint16 port = C2_TELEMETRY_UDP_PORT);
    void stop();
signals:
    void telemetryUpdated(const QVector<C2TelemetryStat> &samples);
    void captureError(const QString &message);
private:
    void drain();
    void publish();
    void closeCapture();
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::unique_ptr<C2TelemetryDecoder> m_decoder;
    bool m_debug = false;
    bool m_useKernelFilter = C2_TELEMETRY_USE_BPF;
    quint64 m_readPackets = 0;
    QString m_interface;
    QString m_lastError;
};

#endif
