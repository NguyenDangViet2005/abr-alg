#ifndef QOSDEBUGPROTOCOL_H
#define QOSDEBUGPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QPair>
#include <QtGlobal>

// Wire format "QS" — the debug channel from XBQoSService to qos-dashboard.
//
// Deliberately SEPARATE from the legacy "NM" format (and from the router's
// telemetry on 12346). Two differences are intentional:
//
//   1. A VERSION byte. NM has none, so its parser has to guess the layout from
//      the payload size (four DATA layouts). The version byte ends that whole
//      class of bug.
//   2. Everything is LITTLE-ENDIAN, ints and floats alike. NM's floats are host
//      order on the device while its ints are network order, which forced the
//      dashboard's `detectScoreFloatEndian` hack. QS has no such split.
//
// Datagram / file record (identical framing in both, so one parser serves both):
//
//   off  len  field
//   0    2    magic       'Q''S' = 0x51 0x53
//   2    1    version     1
//   3    1    recordType  1=SAMPLE 2=EVENT 3=CONFIG
//   4    2    payloadLen  u16 LE
//   6    N    payload
//
// See plans/reports/brainstorm-261002-1345-qos-debug-sender-dashboard.md §3.

constexpr quint16 QS_MAGIC   = 0x5153; // 'Q' 'S'
constexpr quint8  QS_VERSION = 1;

// Datagram header size: magic(2) + version(1) + type(1) + payloadLen(2).
constexpr int QS_HEADER_SIZE = 6;

enum QsRecordType : quint8 {
    QS_SAMPLE = 1,
    QS_EVENT  = 2,
    QS_CONFIG = 3
};

// Why the bitrate moved this tick (§3.1). `NONE` is the v1 default: the ABR
// engine is not wired to this channel yet.
enum QsAbrBranch : quint8 {
    QS_BR_HOLD           = 0,
    QS_BR_UP             = 1,
    QS_BR_HALVE          = 2,
    QS_BR_MUL            = 3,
    QS_BR_PAUSE          = 4,
    QS_BR_NOT_AVAILABLE  = 5,
    QS_BR_TO_STUCK_OBSERVE = 6,
    QS_BR_RESET_REQUEST  = 7
};

enum QsAbrReason : quint8 {
    QS_R_NONE          = 0,
    QS_R_KTO           = 1,
    QS_R_LOSS_HIGH     = 2,
    QS_R_QDELAY_400    = 3,
    QS_R_QDELAY_5      = 4,
    QS_R_TREND_RAPID_DEC = 5,
    QS_R_LINK_DEAD     = 6,
    QS_R_NOT_USABLE    = 7,
    QS_R_STALE         = 8,
    QS_R_COOLDOWN_OK   = 9,
    QS_R_C2_BLOCK_UP   = 10,
    QS_R_SCORE_LOW     = 11,
    QS_R_STUCK_OBSERVE = 12,
    QS_R_RESET_STUCK   = 13
};

enum QsEventType : quint8 {
    QS_EVT_ROUTE_SWITCH        = 1,
    QS_EVT_ABR_PAUSE           = 2,
    QS_EVT_ABR_RESUME          = 3,
    QS_EVT_C2_STATE_CHANGE     = 4,
    QS_EVT_STALE_WARN          = 5,
    QS_EVT_SOCKET_RESET_REQUEST = 6
};

// The 7 raw metrics of ONE flow (video or C2). Kept as a struct, not 7 loose
// floats, so a call site cannot transpose two same-typed fields silently.
struct QsFlow {
    double score   = 0.0;
    double thr     = 0.0;  // throughputKbps — LOAD, not capacity
    double loss    = 0.0;  // percent
    double rtt     = 0.0;
    double minRtt  = 0.0;
    double qdelay  = 0.0;
    double jitter  = 0.0;
};

// One SAMPLE: a single interface's whole state for one tick — the two raw
// flows AND the ABR decision, so the dashboard draws input beside output in
// the same time frame.
struct QsSample {
    qint64  timestampMs = 0;
    quint32 tickSeq     = 0;
    QString iface;
    quint8  priority    = 0;

    bool isActivated = false;
    bool isConnected = false;
    bool isMptcp     = false;
    bool usable      = false;

    bool videoPresent = false;   // a fresh 600 arrived this tick
    bool c2Present    = false;

    quint8 videoTrend   = 255;   // 0..4, 255 = absent
    quint8 c2Trend      = 255;
    quint8 videoQuality = 255;   // 0..7, 255 = absent
    quint8 c2Quality    = 255;
    quint8 c2Mode       = 0;     // 0 FRESH 1 STALE 2 NEVER_SEEN 3 DELAY_ONLY
    quint8 abrBranch    = QS_BR_NOT_AVAILABLE;
    quint8 abrReason    = QS_R_NONE;
    quint8 notUsableTicks = 0;

    qint32 bitrateKbps       = 0;
    qint32 maxAbrKbps        = 0;
    qint32 sinceLastChangeMs = 0;

    QsFlow video;
    QsFlow c2;
};

struct QsEvent {
    qint64  timestampMs = 0;
    QString iface;
    quint8  eventType   = 0;
    QString detail;
};

// The declared topology, sent once at startup so the dashboard knows which
// interfaces to expect and which is primary BEFORE the first sample lands.
struct QsConfig {
    QString appVersion;
    QString primaryIface;
    quint16 udpPort = 0;
    QVector<QPair<QString, quint8>> ifaces; // name, priority
};

// ── Codec (QoSDebugProtocol.cpp) ───────────────────────────────────────────
// Framing and payload encode/decode live as free functions so the encoder can
// be unit-tested without a socket or a sender instance.

// Wrap a payload in the 6-byte QS header.
QByteArray buildQsDatagram(quint8 recordType, const QByteArray &payload);

// True if `buf` starts with the QS magic.
bool isQsDatagram(const QByteArray &buf);

// Read the recordType of a framed datagram, or 0 if too short / bad magic.
quint8 qsRecordType(const QByteArray &datagram);

// Payload encode (no header).
QByteArray encodeQsSample(const QsSample &s);
QByteArray encodeQsEvent(const QsEvent &e);
QByteArray encodeQsConfig(const QsConfig &c);

// Payload decode (no header). Returns false on truncation.
bool decodeQsSample(const QByteArray &payload, QsSample &out);
bool decodeQsEvent(const QByteArray &payload, QsEvent &out);
bool decodeQsConfig(const QByteArray &payload, QsConfig &out);

// ── QSDB file container ──────────────────────────────────────────────────
// Same records as the UDP datagrams, so one decoder serves both (DRY).
//
//   header : 'Q''S''D''B' · u8 version · u32 recordCount
//   body   : repeat [u32 len LE][ <QS datagram verbatim> ]
//
// recordCount is written at close; a crash mid-write leaves it stale, so a
// reader must iterate to EOF rather than trust it (report §8.6).
constexpr char QSDB_MAGIC[4] = { 'Q', 'S', 'D', 'B' };
constexpr int  QSDB_HEADER_SIZE = 9; // magic(4) + version(1) + count(4)
constexpr quint8 QSDB_VERSION = 1;

#endif // QOSDEBUGPROTOCOL_H
