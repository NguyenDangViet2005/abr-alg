#ifndef C2TELEMETRYSTAT_H
#define C2TELEMETRYSTAT_H

#include <QString>
#include <QMetaType>

// Measurements at XBLink, not receiver delivery/command-execution statistics.
// A negative age/RTT means unavailable; never substitute zero for unknown data.
struct C2TelemetryStat {
    QString source;
    QString destination;
    quint32 socketId = 0;
    bool captureAvailable = false;
    bool telemetryValid = false;
    bool heartbeatValid = false;
    bool peerAckValid = false;
    bool rttValid = false;
    bool baselineValid = false;
    bool observationOk = true;
    qint64 telemetryAgeMs = -1;
    qint64 heartbeatAgeMs = -1;
    qint64 ackAgeMs = -1;
    qint64 rttAgeMs = -1;
    double windowSeconds = 0;
    double rttMs = -1;
    double rttSmoothedMs = -1;
    double rttBaselineMs = -1;
    double rttInflationMs = -1;
    double rttRatio = -1;
    double rttTrendMsPerSecond = 0;
    bool rttTrendValid = false;
    quint64 dataPackets = 0;
    quint64 retransmissions = 0;
    quint64 nakReports = 0;
    quint64 captureGaps = 0;
    quint64 captureDrops = 0;
    quint64 crcRejected = 0;
    double retransmissionPercent = -1; // Share of observed transmissions, NOT loss %.
    double nakReportsPerSecond = 0;
    double mavlinkFramesPerSecond = 0;
    double mavlinkKiBps = 0;
    double appRxModelKiBps = 0; // Diagnostic duplicate-count model for PilotApp 10.9.4.

    bool inputsReady() const {
        return captureAvailable && telemetryValid && heartbeatValid && peerAckValid &&
               rttValid && baselineValid && observationOk;
    }
};
Q_DECLARE_METATYPE(C2TelemetryStat)

#endif
