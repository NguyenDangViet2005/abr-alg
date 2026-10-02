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
//   VIDEO / C2   -> 14 '#'-separated fields, see RouterInterfaceStat
//   PRIMARY      -> ONE field: the chosen primary interface name, or "-"
enum class ConnType : int {
    VIDEO   = 0,   // 600
    C2      = 1,   // 601
    PRIMARY = 2    // 602
};

// One VIDEO or C2 row from the router — its own per-interface view of the link.
//
// Distinct from SRTPeerStat, which describes the SRT stream as measured by
// XBSRTFactory on port 12345. These are the ROUTER's measurements, one row per
// interface, arriving on port 12346. The two describe different things at
// different rates and are deliberately NOT merged into one struct.
//
// Field order matches the router's TelemetryModule::formatInterfaceMetrics()
// and the parse site in dev/RouterNetworkHandler.cpp. Changing one without the
// others silently shifts every value.
struct RouterInterfaceStat {
    ConnType connType = ConnType::VIDEO;

    QString interfaceName;      // [0]  e.g. "eth1"
    int     priority      = 0;  // [1]  lower = preferred
    bool    isActivated   = false; // [2]  0/1 on the wire
    bool    isConnected   = false; // [3]  0/1
    bool    isMptcp       = false; // [4]  0/1
    bool    usable        = false; // [5]  0/1 — the router's own verdict
    double  score         = 0.0;   // [6]
    int     trend         = 0;     // [7]  0..4, see trend enum in ScoringModule
    double  throughputKbps = 0.0;  // [8]  kbps, NOT bytes/s
    double  lossRatePct   = 0.0;   // [9]  PERCENT, not a packet count
    double  rttMs         = 0.0;   // [10]
    double  minRttMs      = 0.0;   // [11]
    double  queuingDelayMs = 0.0;  // [12]
    double  jitterMs      = 0.0;   // [13]

    // Set when a field failed to convert, so a consumer can tell "the router
    // reported 0" from "we could not read what the router reported". The raw
    // text is what makes the difference visible; a 0.0 alone never would.
    bool    parseFailed   = false;
};

Q_DECLARE_METATYPE(RouterInterfaceStat)

#endif // ROUTERINTERFACESTAT_H
