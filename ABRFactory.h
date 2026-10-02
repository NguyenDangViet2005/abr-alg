#ifndef ABRFACTORY_H
#define ABRFACTORY_H

#include <QObject>
#include <QVector>
#include "ABRConfigs.h"
#include "SRTAdaptiveBitrateStreaming.h"
#include "SRTPeerStat.h"
#include "RouterInterfaceStat.h"

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
    // handleRouterRow and act here.
    void handleRouterTick(const QString &primaryInterface);

signals:
    void onSrtCameraConnection(const QVector<SRTPeerStat> &peers);
    void onC2TelemetryData(const QVector<SRTPeerStat> &peers);

private:
    static ABRFactory* _instance;
    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;
};

#endif // ABRFACTORY_H
