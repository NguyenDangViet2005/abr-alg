#ifndef ROUTERINTERFACESTAT_H
#define ROUTERINTERFACESTAT_H

#include <QString>
#include <QMetaType>

// Row kind of a router telemetry datagram (UDP id 60, the 3rd byte).
//
// Declared HERE rather than in dev/RouterNetworkHandler.h because the parsed
// struct below carries it, and both are the same thing: the wire contract.
// One definition site, so the enum and the struct cannot drift apart.
//
// This is a LOCAL copy of the enum XBFirm declares in settings/GeneralConfigs.h.
// Kept local on purpose: XBQoSService is a separate binary and must not include
// across the xbfirm repo. The wire values are the contract; if either side ever
// changes a value they must change together.
//
//   VIDEO / C2   -> 22 '#'-separated fields, see RouterInterfaceStat
//   PRIMARY      -> ONE field: the chosen primary interface name, or "-"
//   NETWORK_REF  -> TWO fields: <name>#<strength>, strength 0..4 (weak->strong),
//                   9 = no info. The router reserves this kind but emits nothing
//                   yet (no RSSI source), so it stays silent until a real value
//                   exists — see NetworkReference.h.
//   GPS          -> TWO fields: <lat>#<lon>, %.6f signed. GLOBAL (no name — one
//                   fix for the whole vehicle). No-fix = ABSENT packet. The
//                   router reserves this kind but emits nothing yet (no GPS
//                   source) — see NetworkReference.h.
enum class ConnType : int {
    VIDEO       = 0,   // 600
    C2          = 1,   // 601
    PRIMARY     = 2,   // 602
    NETWORK_REF = 3,   // 603
    GPS         = 4    // 604
};

// One VIDEO or C2 row from the router — its own per-interface view of the link.
//
// Distinct from SRTPeerStat, which describes the SRT stream as measured by
// XBSRTFactory on port 12345. These are the ROUTER's measurements, one row per
// interface, arriving on port 12346. The two describe different things at
// different rates and are deliberately NOT merged into one struct.
//
// LAYOUT: 22 fields = a RAW tcpi counter block in the OLD branch's units
// (fields 7..16, exactly xbfirm's processTCPInfo inputs) followed by the
// refactor's derived metrics (17..22). The raw block is what the port feeds to
// xbfirm's 11-arg processTCPInfo — so no unit fudge and no synthetic counter is
// needed here. See docs/xblink-telemetry-protocol.md §3b (in the router repo).
//
// Field order matches the router's TelemetryModule::formatInterfaceMetrics()
// and the parse site in dev/RouterNetworkHandler.cpp. Changing one without the
// others silently shifts every value.
struct RouterInterfaceStat {
    ConnType connType = ConnType::VIDEO;

    // ── identity / status (fields 1..6) ──────────────────────────────────
    QString interfaceName;      // [1]  e.g. "eth1"
    int     priority      = 0;  // [2]  lower = preferred
    bool    isActivated   = false; // [3]  0/1 on the wire
    bool    isConnected   = false; // [4]  0/1
    bool    isMptcp       = false; // [5]  0/1
    bool    usable        = false; // [6]  0/1 — the router's own verdict

    // ── RAW tcpi counter block (fields 7..16), OLD-branch units ───────────
    // These are xbfirm's processTCPInfo argument list, minus the duplicated rtt.
    quint32 rttUs           = 0;   // [7]  tcpi_rtt, MICROseconds (xbfirm /1000)
    quint64 deliveryRateBps = 0;   // [8]  tcpi_delivery_rate, BYTES/s (xbfirm *8/1000)
    quint32 lost            = 0;   // [9]  tcpi_lost, packets. GAUGE — packets
                                   //      currently considered lost (kernel
                                   //      tp->lost_out), NOT a running total; the
                                   //      raw reading can DECREASE tick-to-tick.
                                   //      Consumed as a per-tick count.
    quint32 segsOut         = 0;   // [10] tcpi_segs_out, packets (cumulative)
    quint32 sndCwnd         = 0;   // [11] tcpi_snd_cwnd, segments
    quint32 sndMss          = 0;   // [12] tcpi_snd_mss, bytes
    quint32 lastAckRecv     = 0;   // [13] tcpi_last_ack_recv, ms
    quint32 sockOutq        = 0;   // [14] idiag_wqueue, bytes — xbfirm's `siocoutq`
    quint32 sockSndBuf      = 0;   // [15] always 0 — not measured; xbfirm never reads it
    quint32 minRttUs        = 0;   // [16] tcpi_min_rtt, MICROseconds (xbfirm /1000)

    // ── refactor-derived metrics (fields 17..22) ──────────────────────────
    double  score         = 0.0;   // [17] 0..100
    int     trend         = 0;     // [18] 0..4, see trend enum in ScoringModule
    double  lossRatePct   = 0.0;   // [19] PERCENT (kept alongside the raw counters)
    double  queuingDelayMs = 0.0;  // [20] ms (drain-time estimate, not RTT−minRtt)
    double  jitterMs      = 0.0;   // [21] ms
    quint64 sockOutqUnacked = 0;   // [22] bytes — in-flight, unacked × sndMss

    // Set when a field failed to convert, so a consumer can tell "the router
    // reported 0" from "we could not read what the router reported". The raw
    // text is what makes the difference visible; a 0.0 alone never would.
    bool    parseFailed   = false;

    // ── Display-unit views of the raw block ──────────────────────────────
    // The wire carries the raw counters only (old units). These convert them to
    // the ms/kbps the log and dashboard read. Deliberately METHODS, not fields:
    // they are derived, and making the conversion explicit stops a reader from
    // mistaking them for independent wire fields. The throughput view is the
    // kernel's own `tcpi_delivery_rate` — which is refreshed only when the
    // kernel generates a rate sample, so it can hold one value for several
    // ticks. That staleness is inherited from the old wire and is what xbfirm
    // itself consumes; do not "fix" it here.
    double rttMs() const          { return rttUs / 1000.0; }
    double minRttMs() const       { return minRttUs / 1000.0; }
    double throughputKbps() const { return deliveryRateBps * 8.0 / 1000.0; }
};

Q_DECLARE_METATYPE(RouterInterfaceStat)

#endif // ROUTERINTERFACESTAT_H
