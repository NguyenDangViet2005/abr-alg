#ifndef QOSDEBUGPROTOCOL_H
#define QOSDEBUGPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QPair>
#include <QtGlobal>
#include "../ABRDecision.h"   // AbrBranch / AbrReason — the wire values' home

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

// Wire magic: the two ASCII bytes 'Q','S' = 0x51 0x53 at offset 0. Written and
// read RAW, never as a u16 — a little-endian u16 write of 0x5153 emits
// 0x53 0x51 ('S','Q') and every reader (spec, dashboard parser, server) would
// reject it. The bug is invisible to a C++ round-trip (writer and reader agree
// with each other) and only a cross-language fixture catches it.
constexpr char QS_MAGIC_BYTES[2] = { 'Q', 'S' };
constexpr quint8  QS_VERSION = 1;

// Datagram header size: magic(2) + version(1) + type(1) + payloadLen(2).
constexpr int QS_HEADER_SIZE = 6;

enum QsRecordType : quint8 {
    QS_SAMPLE = 1,
    QS_EVENT  = 2,
    QS_CONFIG = 3
};

// Why the bitrate moved this tick (§3.1).
//
// The values now live in ABRDecision.h (AbrBranch / AbrReason) so the DECIDER
// and the WIRE share one definition — the reason a branch was taken is computed
// where the decision is made, not reconstructed later. These QS_* names are
// aliases kept for the encoder, the decoder and the tests, which read the wire
// vocabulary. `ABR_NOT_AVAILABLE` is the pre-wiring default (no decision yet).
enum QsAbrBranch : quint8 {
    QS_BR_HOLD             = static_cast<quint8>(AbrBranch::HOLD),
    QS_BR_UP               = static_cast<quint8>(AbrBranch::UP),
    QS_BR_HALVE            = static_cast<quint8>(AbrBranch::HALVE),
    QS_BR_MUL              = static_cast<quint8>(AbrBranch::MUL),
    QS_BR_PAUSE            = static_cast<quint8>(AbrBranch::PAUSE),
    QS_BR_NOT_AVAILABLE    = static_cast<quint8>(AbrBranch::NOT_AVAILABLE),
    QS_BR_TO_STUCK_OBSERVE = static_cast<quint8>(AbrBranch::TO_STUCK_OBSERVE),
    QS_BR_RESET_REQUEST    = static_cast<quint8>(AbrBranch::RESET_REQUEST)
};

enum QsAbrReason : quint8 {
    QS_R_NONE            = static_cast<quint8>(AbrReason::NONE),
    QS_R_KTO             = static_cast<quint8>(AbrReason::KTO),
    QS_R_LOSS_HIGH       = static_cast<quint8>(AbrReason::LOSS_HIGH),
    QS_R_QDELAY_400      = static_cast<quint8>(AbrReason::QDELAY_400),
    QS_R_QDELAY_5        = static_cast<quint8>(AbrReason::QDELAY_5),
    QS_R_TREND_RAPID_DEC = static_cast<quint8>(AbrReason::TREND_RAPID_DEC),
    QS_R_LINK_DEAD       = static_cast<quint8>(AbrReason::LINK_DEAD),
    QS_R_NOT_USABLE      = static_cast<quint8>(AbrReason::NOT_USABLE),
    QS_R_STALE           = static_cast<quint8>(AbrReason::STALE),
    QS_R_COOLDOWN_OK     = static_cast<quint8>(AbrReason::COOLDOWN_OK),
    QS_R_C2_BLOCK_UP     = static_cast<quint8>(AbrReason::C2_BLOCK_UP),
    QS_R_SCORE_LOW       = static_cast<quint8>(AbrReason::SCORE_LOW),
    QS_R_STUCK_OBSERVE   = static_cast<quint8>(AbrReason::STUCK_OBSERVE),
    QS_R_RESET_STUCK     = static_cast<quint8>(AbrReason::RESET_STUCK)
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
