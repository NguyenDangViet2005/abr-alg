#ifndef SIYILINKSTAT_H
#define SIYILINKSTAT_H

#include <QtGlobal>

struct SiyiLinkStat {
    uint16_t sequence = 0;
    int signal = 0;         // 0 - 100%
    int inactiveTime = 0;   // ms
    int upstream = 0;       // Bytes/s or kbps
    int downstream = 0;     // Bytes/s or kbps
    int txBandwidth = 0;    // Raw RF TX bandwidth reported by SIYI
    int rxBandwidth = 0;    // Raw RF RX bandwidth reported by SIYI
    int rssi = -100;        // dBm (e.g. -60, -75, -85)
    int frequency = 0;      // MHz
    int channel = 0;
    qint64 timestampMs = 0;
    bool isAlive = false;
};

#endif // SIYILINKSTAT_H
