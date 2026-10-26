#ifndef MAVLINKSTREAM_H
#define MAVLINKSTREAM_H

#include <QByteArray>
#include <QVector>

struct MavlinkFrameInfo {
    quint32 messageId;
    quint8 systemId;
    quint8 componentId;
};

// CRC verification and framing only. ABR does not require flight/sensor fields.
// Signed frames are CRC checked; their cryptographic signature is not verified.
class MavlinkStream {
public:
    QVector<MavlinkFrameInfo> feed(const QByteArray &bytes);
    void reset() { m_buffer.clear(); }
    quint64 crcRejected() const { return m_crcRejected; }
    quint64 framesValid() const { return m_framesValid; }
    int pendingBytes() const { return int(m_buffer.size()); }
private:
    QByteArray m_buffer;
    quint64 m_crcRejected = 0;
    quint64 m_framesValid = 0;
};

#endif
