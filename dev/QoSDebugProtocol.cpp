#include "QoSDebugProtocol.h"
#include <QDataStream>
#include <QIODevice>

// ── Low-level string helpers ────────────────────────────────────────────────
// Strings are length-prefixed UTF-8. iface names use a u8 length; event detail
// uses u16 (it can exceed 255). Long names are truncated rather than refused —
// the writer clamps, so the reader never has to guess.

namespace {

void writeStr8(QDataStream &ds, const QString &s)
{
    QByteArray utf8 = s.toUtf8();
    if (utf8.size() > 255) utf8.truncate(255);
    ds << static_cast<quint8>(utf8.size());
    ds.writeRawData(utf8.constData(), utf8.size());
}

void writeStr16(QDataStream &ds, const QString &s)
{
    QByteArray utf8 = s.toUtf8();
    if (utf8.size() > 65535) utf8.truncate(65535);
    ds << static_cast<quint16>(utf8.size());
    ds.writeRawData(utf8.constData(), utf8.size());
}

QString readStr8(QDataStream &ds)
{
    quint8 len = 0;
    ds >> len;
    if (ds.status() != QDataStream::Ok || len == 0) return QString();
    QByteArray buf(len, Qt::Uninitialized);
    if (ds.readRawData(buf.data(), len) != len) return QString();
    return QString::fromUtf8(buf);
}

QString readStr16(QDataStream &ds)
{
    quint16 len = 0;
    ds >> len;
    if (ds.status() != QDataStream::Ok || len == 0) return QString();
    QByteArray buf(len, Qt::Uninitialized);
    if (ds.readRawData(buf.data(), len) != len) return QString();
    return QString::fromUtf8(buf);
}

// Configure a QDataStream for the QS contract: little-endian, single-precision
// floats. A default QDataStream is BIG-endian, so this is not optional —
// forgetting it is the exact bug the format was designed to avoid.
//
// Configured IN PLACE rather than returned: QDataStream is non-copyable and
// non-movable in Qt5 (Q_DISABLE_COPY), so a by-value factory does not compile.
void configureQsStream(QDataStream &ds)
{
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
}

} // namespace

// ── Framing ─────────────────────────────────────────────────────────────────

QByteArray buildQsDatagram(quint8 recordType, const QByteArray &payload)
{
    QByteArray out;
    out.reserve(QS_HEADER_SIZE + payload.size());
    QDataStream ds(&out, QIODevice::WriteOnly);
    configureQsStream(ds);
    ds.writeRawData(QS_MAGIC_BYTES, 2);   // raw 'Q','S' — NOT a u16 (LE would swap)
    ds << QS_VERSION;                     // u8
    ds << recordType;                     // u8
    ds << static_cast<quint16>(payload.size()); // u16 LE
    ds.writeRawData(payload.constData(), payload.size());
    return out;
}

bool isQsDatagram(const QByteArray &buf)
{
    if (buf.size() < QS_HEADER_SIZE) return false;
    // Read the magic RAW too, so the check matches the writer byte-for-byte
    // regardless of stream byte order.
    return buf.at(0) == QS_MAGIC_BYTES[0] && buf.at(1) == QS_MAGIC_BYTES[1];
}

quint8 qsRecordType(const QByteArray &datagram)
{
    if (!isQsDatagram(datagram)) return 0;
    return static_cast<quint8>(datagram.at(3)); // type byte after magic(2)+version(1)
}

// ── SAMPLE ──────────────────────────────────────────────────────────────────

QByteArray encodeQsSample(const QsSample &s)
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    configureQsStream(ds);

    ds << s.timestampMs;                   // i64
    ds << s.tickSeq;                       // u32
    writeStr8(ds, s.iface);                // u8 len + utf8
    ds << s.priority;                      // u8

    quint8 flags = 0;
    if (s.isActivated) flags |= 0x01;
    if (s.isConnected) flags |= 0x02;
    if (s.isMptcp)     flags |= 0x04;
    if (s.usable)      flags |= 0x08;
    ds << flags;

    ds << static_cast<quint8>(s.videoPresent ? 1 : 0);
    ds << static_cast<quint8>(s.c2Present ? 1 : 0);
    ds << s.videoTrend;
    ds << s.c2Trend;
    ds << s.videoQuality;
    ds << s.c2Quality;
    ds << s.c2Mode;
    ds << s.abrBranch;
    ds << s.abrReason;
    ds << s.notUsableTicks;

    ds << s.bitrateKbps;
    ds << s.maxAbrKbps;
    ds << s.ceilingKbps;                   // i32, v2 — after maxAbrKbps
    ds << s.capKbps;                       // i32, v3 — after ceilingKbps
    ds << s.sinceLastChangeMs;

    // VIDEO then C2, each the same 7 fields in a fixed order.
    ds << static_cast<float>(s.video.score);
    ds << static_cast<float>(s.video.thr);
    ds << static_cast<float>(s.video.loss);
    ds << static_cast<float>(s.video.rtt);
    ds << static_cast<float>(s.video.minRtt);
    ds << static_cast<float>(s.video.qdelay);
    ds << static_cast<float>(s.video.jitter);
    ds << static_cast<float>(s.c2.score);
    ds << static_cast<float>(s.c2.thr);
    ds << static_cast<float>(s.c2.loss);
    ds << static_cast<float>(s.c2.rtt);
    ds << static_cast<float>(s.c2.minRtt);
    ds << static_cast<float>(s.c2.qdelay);
    ds << static_cast<float>(s.c2.jitter);

    return out;
}

bool decodeQsSample(const QByteArray &payload, QsSample &s)
{
    QDataStream ds(payload);
    configureQsStream(ds);

    ds >> s.timestampMs;
    ds >> s.tickSeq;
    s.iface = readStr8(ds);
    ds >> s.priority;

    quint8 flags = 0;
    ds >> flags;
    s.isActivated = (flags & 0x01) != 0;
    s.isConnected = (flags & 0x02) != 0;
    s.isMptcp     = (flags & 0x04) != 0;
    s.usable      = (flags & 0x08) != 0;

    quint8 videoPresent = 0, c2Present = 0;
    ds >> videoPresent >> c2Present;
    s.videoPresent = videoPresent != 0;
    s.c2Present    = c2Present != 0;

    ds >> s.videoTrend >> s.c2Trend >> s.videoQuality >> s.c2Quality;
    ds >> s.c2Mode >> s.abrBranch >> s.abrReason >> s.notUsableTicks;
    ds >> s.bitrateKbps >> s.maxAbrKbps >> s.ceilingKbps >> s.capKbps >> s.sinceLastChangeMs;

    float v[7] = {0};
    float c[7] = {0};
    for (float &f : v) ds >> f;
    for (float &f : c) ds >> f;
    s.video = QsFlow{ v[0], v[1], v[2], v[3], v[4], v[5], v[6] };
    s.c2    = QsFlow{ c[0], c[1], c[2], c[3], c[4], c[5], c[6] };

    return ds.status() == QDataStream::Ok;
}

// ── EVENT ───────────────────────────────────────────────────────────────────

QByteArray encodeQsEvent(const QsEvent &e)
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    configureQsStream(ds);
    ds << e.timestampMs;
    writeStr8(ds, e.iface);
    ds << e.eventType;
    writeStr16(ds, e.detail);
    return out;
}

bool decodeQsEvent(const QByteArray &payload, QsEvent &e)
{
    QDataStream ds(payload);
    configureQsStream(ds);
    ds >> e.timestampMs;
    e.iface = readStr8(ds);
    ds >> e.eventType;
    e.detail = readStr16(ds);
    return ds.status() == QDataStream::Ok;
}

// ── CONFIG ──────────────────────────────────────────────────────────────────

QByteArray encodeQsConfig(const QsConfig &c)
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    configureQsStream(ds);
    writeStr8(ds, c.appVersion);
    ds << static_cast<quint8>(c.ifaces.size());
    for (const auto &pair : c.ifaces) {
        writeStr8(ds, pair.first);
        ds << pair.second;                 // priority
    }
    writeStr8(ds, c.primaryIface);
    ds << c.udpPort;
    return out;
}

bool decodeQsConfig(const QByteArray &payload, QsConfig &c)
{
    QDataStream ds(payload);
    configureQsStream(ds);
    c.appVersion = readStr8(ds);
    quint8 count = 0;
    ds >> count;
    if (ds.status() != QDataStream::Ok) return false;
    c.ifaces.clear();
    c.ifaces.reserve(count);
    for (quint8 i = 0; i < count; ++i) {
        QString name = readStr8(ds);
        quint8 priority = 0;
        ds >> priority;
        c.ifaces.append({ name, priority });
    }
    c.primaryIface = readStr8(ds);
    ds >> c.udpPort;
    return ds.status() == QDataStream::Ok;
}
