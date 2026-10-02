#ifndef QOSDEBUGSENDER_H
#define QOSDEBUGSENDER_H

#include <QObject>
#include <QHash>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QFile>
#include "../RouterInterfaceStat.h"
#include "QoSDebugProtocol.h"

class QUdpSocket;

// The QoS debug channel: ONE class with two jobs, deliberately separate.
//
//   Job 1 — structured qDebug, ALWAYS. Field-explicit lines so a log is
//           readable without a decoder. Independent of the enabled flag.
//   Job 2 — when enabled, stream the same records over UDP to qos-dashboard
//           and mirror them into a `.qsdb` file for offline import.
//
// It READS and PUBLISHES. It never decides and never feeds a signal back into
// the QoS path — that is what keeps it safe to tap any signal.
//
// See plans/reports/brainstorm-261002-1345-qos-debug-sender-dashboard.md.
class QoSDebugSender : public QObject
{
    Q_OBJECT
public:
    explicit QoSDebugSender(QObject *parent = nullptr);
    ~QoSDebugSender() override;

    // Default OFF: no UDP, no file. qDebug still runs. Turning this on is a
    // CLI decision in main.cpp — XBQoSService has no config file, and one must
    // NOT be added (the router owns config.ini; regenerating it is forbidden).
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    // One-time identity block (VERSION, listening iface, port). Cheap; safe to
    // call before startUdp.
    void setAppVersion(const QString &version) { m_appVersion = version; }
    void setTopology(const QVector<QPair<QString, quint8>> &ifaces,
                     const QString &primaryIface, quint16 udpPort);

    // Open the dashboard UDP channel. `recordType` is always QS_SAMPLE/EVENT/
    // CONFIG behind these calls, so only the destination is needed.
    bool startUdp(const QString &host, quint16 port);
    void stopUdp();

    // Open a `dir/qos-session-YYYYMMDD-HHMMSS.qsdb` file. No-op if `dir` is
    // empty or cannot be opened (logged, not fatal).
    void startFileLog(const QString &dir);
    void stopFileLog();
    bool isFileLogging() const { return m_file.isOpen(); }

    // Emit the CONFIG record now (if enabled). Called once topology is known.
    void emitConfig();

public slots:
    // Accumulate one row for its interface. Arrives before the 602 tick.
    void handleRouterRow(const RouterInterfaceStat &row);

    // End-of-tick: for every interface seen this tick, merge VIDEO + C2 + the
    // (possibly absent) ABR decision into one SAMPLE, log it, and — if enabled
    // — publish it.
    void handleTick(const QString &primaryInterface);

    // ABR decision hook — connected to XBAdaptiveBitrateStreaming::decisionMade
    // (see XBQoSService::setupConnections). Stores the latest decision per
    // interface so the next SAMPLE carries the branch/reason/maxAbr beside the
    // inputs. Until the first decision arrives, samples say NOT_AVAILABLE.
    void handleAbrDecision(const QString &iface, int kbps, int c2Mode,
                           int branch, int reason, int maxAbr);

    // A discrete event (route switch, pause, stuck-reset request, …).
    void handleEvent(const QString &iface, quint8 type, const QString &detail);

signals:
    // A record could not be sent (socket closed / not enabled / OS error).
    // Emitted at most once per window so a dead receiver cannot spam the log.
    void onDropped(quint8 recordType, quint16 bytes);

private:
    struct AbrState {
        int bitrateKbps = 0;
        int c2Mode = 0;
        int branch = static_cast<int>(QS_BR_NOT_AVAILABLE);
        int reason = static_cast<int>(QS_R_NONE);
        int maxAbrKbps = 0;
        bool present = false;
    };

    void emitSample(const QString &iface, const QString &primaryIface);
    void writeRecord(quint8 recordType, const QByteArray &datagram);

    QsSample buildSample(const QString &iface) const;
    void logSample(const QsSample &s) const;
    void logEvent(const QsEvent &e) const;

    // Per-tick accumulators. Keyed by interface name; cleared each tick.
    QHash<QString, RouterInterfaceStat> m_video;
    QHash<QString, RouterInterfaceStat> m_c2;
    // Latest ABR decision per interface (persists across ticks until updated).
    QHash<QString, AbrState> m_decision;

    QString m_primaryIface;
    QString m_appVersion = QStringLiteral("dev");
    QVector<QPair<QString, quint8>> m_topology;

    bool m_enabled = false;
    QElapsedTimer m_clock;
    quint32 m_seq = 0;

    QUdpSocket *m_udp = nullptr;
    QHostAddress m_host;
    quint16 m_port = 0;

    QFile m_file;
    quint32 m_fileCount = 0;

    // Rate-limits the onDropped signal (8 s), so a down dashboard does not
    // turn every tick into a log line.
    QElapsedTimer m_dropLog;
};

#endif // QOSDEBUGSENDER_H
