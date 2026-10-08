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
    // The effective one is relayed on routerAbrEffectiveBitrate, which is what
    // drives the camera — no longer log-only.
    void handleRouterAbrBitrate(const QString &iface, unsigned int kbps);

    // The camera's report of what it actually achieved for a request. Routed to
    // the PRIMARY interface's instance (it owns the camera). Closes the loop.
    void handleCameraReportedBitrate(int achievedKbps);

signals:
    void onSrtCameraConnection(const QVector<SRTPeerStat> &peers);
    void onC2TelemetryData(const QVector<SRTPeerStat> &peers);

    // The bitrate the PRIMARY interface wants, for the camera. Distinct from
    // routerAbrDecision (which carries EVERY instance's decision for the debug
    // channel): this carries only the effective one, so a consumer driving the
    // camera cannot act on a spare interface's proposal.
    void routerAbrEffectiveBitrate(int kbps);

    // Relay of a per-interface router-ABR decision (XBAdaptiveBitrateStreaming::
    // decisionMade), so a consumer can tap the decision without holding a
    // pointer to every instance. The QS debug channel connects here.
    void routerAbrDecision(const QString &iface, int kbps, int c2Mode,
                           int branch, int reason, int maxAbr, int ceiling);

    // Relay of the instant a ceiling is learned (XBAdaptiveBitrateStreaming::
    // ceilingChanged). Edge-triggered, so the QS channel can fire the ceiling
    // EVENT without waiting for the next per-tick decision.
    void routerAbrCeiling(const QString &iface, unsigned int kbps);

private:
    static ABRFactory* _instance;
    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;

    QHash<QString, XBAdaptiveBitrateStreaming*> _routerAbrs;
    QString _primaryIface;
};

#endif // ABRFACTORY_H
