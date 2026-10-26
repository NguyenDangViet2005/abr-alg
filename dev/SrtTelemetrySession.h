#ifndef SRTTELEMETRYSESSION_H
#define SRTTELEMETRYSESSION_H

#include "MavlinkStream.h"
#include "../C2TelemetryStat.h"
#include <deque>
#include <map>
#include <optional>

namespace XbTelemetry {

struct Counts {
    quint64 data = 0, retransmits = 0, nak = 0, bytes = 0, chunks = 0;
    quint64 frames = 0, gaps = 0, invalid = 0;
};

class RttTracker {
public:
    void add(qint64 now, double rtt, qint64 staleMs);
    void snapshot(C2TelemetryStat &out, qint64 now, qint64 staleMs, double elapsed);
private:
    std::deque<std::pair<qint64, double>> m_samples;
    qint64 m_last = -1, m_previousReport = -1;
    double m_smoothed = -1, m_previousSmoothed = -1;
};

struct Session {
    QString source, destination, sourceIp, destinationIp, pairKey;
    bool outgoing = false;
    quint32 socketId = 0;
    quint64 order = 0;
    qint64 created = 0, lastSeen = 0, lastFc = -1, lastHeartbeat = -1, lastAck = -1;
    Counts counts, previous;
    quint64 previousCrcRejected = 0;
    MavlinkStream parser;
    RttTracker rtt;
    QString lastMavlinkSource;
    QByteArray lastXbPrefix;
    quint64 encryptedPackets = 0;

    void receive(const QByteArray &payload, qint64 now, quint8 systemId, quint8 componentId, qint64 staleMs);
    void flush(qint64 now, quint8 systemId, quint8 componentId);
private:
    struct Pending { qint64 time; quint32 flags; QByteArray body; };
    std::map<quint32, Pending> m_pending;
    std::optional<quint32> m_expected, m_messageNumber;
    QByteArray m_parts;
    void gap(quint32 count);
    void application(const QByteArray &body, qint64 time, quint8 systemId, quint8 componentId);
};

}
#endif
