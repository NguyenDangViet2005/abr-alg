#ifndef SRTPEERSTAT_H
#define SRTPEERSTAT_H

#include <QString>
#include <QMetaType>

struct SRTPeerStat {
    QString peerAddress;
    int peerPort = 0;
    qint64 msTimeStamp = 0;
    qint64 pktSentTotal = 0;
    qint64 pktRecvTotal = 0;
    int pktSndLossTotal = 0;
    int pktRcvLossTotal = 0;
    int pktRetransTotal = 0;
    qint64 byteSentTotal = 0;
    qint64 byteRecvTotal = 0;
    double mbpsSendRate = 0.0;
    double mbpsRecvRate = 0.0;
    double msRTT = 0.0;
    double mbpsBandwidth = 0.0;
    int pktSndDropTotal = 0;
    int pktRcvDropTotal = 0;
};

Q_DECLARE_METATYPE(SRTPeerStat)

#endif // SRTPEERSTAT_H
