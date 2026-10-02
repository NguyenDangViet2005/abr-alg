#ifndef ABRFACTORY_H
#define ABRFACTORY_H

#include <QObject>
#include <QVector>
#include <QHash>
#include <QStringList>
#include "ABRConfigs.h"
#include "SRTAdaptiveBitrateStreaming.h"
#include "SRTPeerStat.h"
#include "RouterInterfaceStat.h"

class XBAdaptiveBitrateStreaming;

class ABRFactory : public QObject
{
    Q_OBJECT
public:
    explicit ABRFactory(QObject *parent = nullptr);
    static ABRFactory* instance();
    void init();
    SRTAdaptiveBitrateStreaming* srtAbr() const { return _srtAdaptiveBitrateStreaming; }

public slots:
    void startCameraSocketAbr();
    void stopCameraSocketAbr();
    void handleC2Data(const QVector<SRTPeerStat> &peers);

    // Creates one XBAdaptiveBitrateStreaming per interface and connects it back.
    // `isC2Configured` is a GLOBAL fact (is C2 deployed at all), passed to every
    // instance's ctor — an instance cannot tell "C2 not deployed" from "this
    // link's C2 subflow died" on its own. See report §6b.
    void startRouterAbr(const QStringList &ifaces, bool isC2Configured);
    void stopRouterAbr();

    // The router's own per-interface view, from RouterNetworkHandler (UDP
    // 12346, id-60 rows). Distinct from handleC2Data above, which carries
    // XBSRTFactory's QoS measurements of the SRT stream on port 12345 — the
    // two describe different things and are kept in separate slots so neither
    // can be mistaken for the other.
    //
    // NOTHING ACTS ON THIS YET. It logs, so the router's numbers can be read
    // beside the SRT ones before any bitrate decision reads them. The moment a
    // decision does read them, that split is gone — so keep the slot thin and
    // route the decision through a named accessor instead.
    void handleRouterRow(const RouterInterfaceStat &row);

    // End-of-tick marker (row 602). The router sends it last, so a consumer
    // wanting one consistent snapshot per tick should accumulate in
    // handleRouterRow and act here. Fans out to EVERY instance; each decides
    // whether it is the primary.
    void handleRouterTick(const QString &primaryInterface);

    // A per-interface router-ABR proposal. Only the PRIMARY interface's
    // proposal is effective; the others are logged as spare (future failover).
    void handleRouterAbrBitrate(const QString &iface, unsigned int kbps);

    // Instance status line (currently informational).
    void handleRouterAbrStatus(const QString &iface, int status);

signals:
    void onSrtCameraConnection(const QVector<SRTPeerStat> &peers);
    void onC2TelemetryData(const QVector<SRTPeerStat> &peers);

private:
    static ABRFactory* _instance;
    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;

    QHash<QString, XBAdaptiveBitrateStreaming*> _routerAbrs;
    QString _primaryIface;
};

#endif // ABRFACTORY_H
