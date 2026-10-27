#ifndef C2TELEMETRYLOG_H
#define C2TELEMETRYLOG_H

#include "C2TelemetryPolicy.h"
#include "ABRConfigs.h"
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>

// Formatting only: age cached samples without advancing policy or decoder windows.
inline QString nativeC2Log(const QVector<C2TelemetryStat> &samples, qint64 elapsed,
                           const C2TelemetryPolicy::Decision &decision)
{
    auto age = [elapsed](qint64 value) { return value < 0 ? qint64(-1) : value + qMax(qint64(0), elapsed); };
    auto fresh = [&age](bool valid, qint64 value) {
        return valid && value >= 0 && age(value) <= C2_TELEMETRY_STALE_MS;
    };
    bool capture = !samples.isEmpty();
    for (const auto &sample : samples) capture = capture && sample.captureAvailable;
    const QString status = !capture ? "CAPTURE_ERROR" : decision.signal ? "LIVE" : "NO_SIGNAL";
    const QString level = decision.level == C2TelemetryPolicy::Critical ? "CRITICAL" :
                          decision.level == C2TelemetryPolicy::High ? "HIGH" : "NORMAL";
    const QString cap = decision.ceilingKbps == std::numeric_limits<unsigned int>::max()
                      ? "none" : QString::number(decision.ceilingKbps);
    QString line = QString(" | C2: %1 priority=%2 hold=%3 cap=%4")
        .arg(status, level).arg(decision.holdIncrease).arg(cap);
    double rx = 0, mav = 0, frames = 0, rttMs = -1, retrans = -1, nak = 0;
    qint64 fcAge = -1, hbAge = -1;
    bool missingHeartbeat = samples.isEmpty();
    for (const auto &sample : samples) {
        const bool fc = fresh(sample.telemetryValid, sample.telemetryAgeMs);
        const bool ack = fresh(sample.peerAckValid, sample.ackAgeMs);
        const bool rtt = ack && fresh(sample.rttValid, sample.rttAgeMs);
        if (fc) {
            rx += sample.appRxModelKiBps;
            mav += sample.mavlinkKiBps;
            frames += sample.mavlinkFramesPerSecond;
        }
        fcAge = qMax(fcAge, age(sample.telemetryAgeMs));
        hbAge = qMax(hbAge, age(sample.heartbeatAgeMs));
        missingHeartbeat = missingHeartbeat || sample.heartbeatAgeMs < 0;
        if (rtt) rttMs = qMax(rttMs, sample.rttSmoothedMs);
        if (elapsed <= C2_TELEMETRY_STALE_MS) {
            retrans = qMax(retrans, sample.retransmissionPercent);
            nak += sample.nakReportsPerSecond;
        }
    }
    if (qgetenv("XB_C2_DEBUG") != "1") {
        QString brief = QString(" | C2: %1 priority=%2").arg(status, level);
        if (rttMs >= 0) brief += QString(" RTT=%1ms").arg(rttMs, 0, 'f', 0);
        brief += QString(" RX_APP=%1kBps").arg(rx, 0, 'f', 2);
        if (retrans >= 2.5) brief += QString(" RTX=%1%").arg(retrans, 0, 'f', 1);
        if (nak >= 3) brief += QString(" NAK=%1/s").arg(nak, 0, 'f', 1);
        if (decision.holdIncrease) brief += " HOLD";
        if (cap != "none") brief += " cap=" + cap;
        return brief;
    }
    // Compact summary: sum rates, show worst RTT/retransmission/ages. Full
    // endpoints, socket and validity flags remain in opt-in C2 SESSION debug.
    const QString peer = samples.size() == 1 && !samples.front().destination.isEmpty()
                       ? samples.front().destination : QString("multi(%1)").arg(samples.size());
    line += QString(" RTT=%1ms RTX=%2% NAK=%3/s RX_APP=%4kBps MAV=%5KiB/s frames=%6/s age=%7ms HB=%8ms ready=%9 peer=%10")
        .arg(rttMs, 0, 'f', 0).arg(retrans, 0, 'f', 1).arg(nak, 0, 'f', 1)
        .arg(rx, 0, 'f', 2).arg(mav, 0, 'f', 2).arg(frames, 0, 'f', 0)
        .arg(fcAge).arg(missingHeartbeat ? -1 : hbAge).arg(decision.inputsReady).arg(peer);
    return line;
}
// Machine-readable monitor data is independent of the compact console text.
inline QJsonObject nativeC2Monitor(const QVector<C2TelemetryStat> &samples, qint64 elapsed,
                                  const C2TelemetryPolicy::Decision &decision)
{
    auto age = [elapsed](qint64 value) { return value < 0 ? qint64(-1) : value + qMax(qint64(0), elapsed); };
    auto fresh = [&age](bool valid, qint64 value) { return valid && value >= 0 && age(value) <= C2_TELEMETRY_STALE_MS; };
    bool capture = !samples.isEmpty();
    double rx = 0, mav = 0, frames = 0;
    qint64 fcAge = -1, hbAge = -1;
    bool missingHb = samples.isEmpty();
    QJsonArray sessions;
    for (const auto &sample : samples) {
        capture = capture && sample.captureAvailable;
        const bool fc = fresh(sample.telemetryValid, sample.telemetryAgeMs);
        const bool ack = fresh(sample.peerAckValid, sample.ackAgeMs);
        const bool rtt = ack && fresh(sample.rttValid, sample.rttAgeMs);
        if (fc) { rx += sample.appRxModelKiBps; mav += sample.mavlinkKiBps; frames += sample.mavlinkFramesPerSecond; }
        fcAge = qMax(fcAge, age(sample.telemetryAgeMs));
        hbAge = qMax(hbAge, age(sample.heartbeatAgeMs));
        missingHb = missingHb || sample.heartbeatAgeMs < 0;
        QJsonObject row;
        row["source"] = sample.source; row["peer"] = sample.destination; row["socket"] = double(sample.socketId);
        row["signal"] = fc; row["ack"] = ack;
        row["rtt_ms"] = rtt ? sample.rttSmoothedMs : -1;
        row["ready"] = fc && fresh(sample.heartbeatValid, sample.heartbeatAgeMs) && rtt && sample.baselineValid && sample.observationOk;
        sessions.append(row);
    }
    const bool live = capture && decision.signal;
    QJsonObject data;
    data["signal"] = live;
    data["status"] = !capture ? "capture_error" : live ? "receiving" : "no_signal";
    data["peer"] = samples.size() == 1 ? QJsonValue(samples.front().destination) :
                     samples.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString("%1 sessions").arg(samples.size()));
    data["rx_app_kBps"] = live ? QJsonValue(rx) : QJsonValue(QJsonValue::Null);
    data["mavlink_KiBps"] = live ? QJsonValue(mav) : QJsonValue(QJsonValue::Null);
    data["frames_hz"] = live ? QJsonValue(frames) : QJsonValue(QJsonValue::Null);
    data["age_ms"] = live && fcAge >= 0 ? QJsonValue(double(fcAge)) : QJsonValue(QJsonValue::Null);
    data["heartbeat_ms"] = live && !missingHb ? QJsonValue(double(hbAge)) : QJsonValue(QJsonValue::Null);
    data["priority"] = decision.level == C2TelemetryPolicy::Critical ? "CRITICAL" : decision.level == C2TelemetryPolicy::High ? "HIGH" : "NORMAL";
    data["hold_increase"] = decision.holdIncrease;
    data["ceiling_kbps"] = decision.ceilingKbps == std::numeric_limits<unsigned int>::max()
                         ? QJsonValue(QJsonValue::Null) : QJsonValue(double(decision.ceilingKbps));
    data["sessions"] = sessions;
    return data;
}
#endif
