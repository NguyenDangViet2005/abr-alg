#ifndef ABRFACTORY_H
#define ABRFACTORY_H

#include <QObject>
#include <QVector>
#include <QHash>
#include <QStringList>
#include <QElapsedTimer>
#include "ABRConfigs.h"
#include "ABRTuning.h"   // ABR_SWITCH_DEBOUNCE_MS
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

    // Phase 09 (headroom): the steady-state cap percent, applied to every router
    // ABR instance at creation (startRouterAbr). Set from main.cpp's
    // --abr-headroom before start(); defaults to ABR_HEADROOM_FACTOR_PCT. Router
    // ABR only — the SRT/camera engine keeps its own 0.85 headroom.
    void setHeadroomPct(int pct);

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

    // A per-interface radio strength (row 603). Routed by name to the owning
    // instance's NetworkReference, exactly as handleRouterRow is. An unmatched
    // name is the declared-vs-seen mismatch — warn, do not swallow.
    void handleNetworkStrength(const QString &name, int strength);

    // A GLOBAL GPS fix (row 604). GPS carries no interface name — the same fix
    // is true for every interface — so this BROADCASTS to every instance rather
    // than routing by name. Each instance's NetworkReference keeps its own
    // anchor (the position at ITS ceiling-learn), so the verdict stays per-link.
    void handleGpsFix(double lat, double lon);

    // A per-interface router-ABR proposal. Only the PRIMARY interface's
    // proposal is effective; the others are logged as spare (future failover).
    // The effective one is relayed on routerAbrEffectiveBitrate, which is what
    // drives the camera — no longer log-only.
    void handleRouterAbrBitrate(const QString &iface, unsigned int kbps);

    // The camera's report of what it actually achieved for a request. Routed to
    // the PRIMARY interface's instance (it owns the camera). Closes the loop.
    void handleCameraReportedBitrate(int achievedKbps);

    // The last NON-ZERO bitrate the factory relayed for the PRIMARY interface —
    // i.e. the value the camera was last told to run. Sentinel -1 = never
    // dispatched. While --disable-abr is on, XBQoSService::m_currentBitrate
    // stays 0 (the mute is upstream of it), so the loss-driven socket-restart
    // guard reads THIS instead: a camera collapse must still be able to trigger
    // a restart while the bitrate itself is frozen.
    int lastEffectiveBitrate() const { return _lastEffectiveKbps; }

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
                           int branch, int reason, int maxAbr, int ceiling, int cap);

    // Relay of the instant a ceiling is learned (XBAdaptiveBitrateStreaming::
    // ceilingChanged). Edge-triggered, so the QS channel can fire the ceiling
    // EVENT without waiting for the next per-tick decision.
    void routerAbrCeiling(const QString &iface, unsigned int kbps);

    // Relay of a per-interface router-ABR stream-reset request (Phase 11; the
    // >70% loss-ladder band). The instance owns no socket, so the executor lives
    // in XBQoSService, which maps this to CameraControl::requestStreamRefresh.
    // Already rate-limited at the instance's emit site.
    void routerAbrResetRequested(const QString &iface, const QString &reason);

private:
    static ABRFactory* _instance;
    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;

    QHash<QString, XBAdaptiveBitrateStreaming*> _routerAbrs;
    QString _primaryIface;
    // Phase 09 (headroom): steady-state cap percent, applied per instance at
    // creation. Default matches the compile-time #define.
    int _headroomPct = ABR_HEADROOM_FACTOR_PCT;

    // ── Primary-role adopt state (PORT DEVIATION) ────────────────────────
    // m_lastEffectiveKbps: the last NON-ZERO bitrate this factory told the CAMERA
    // to run (= the value relayed on routerAbrEffectiveBitrate = XBQoSService::
    // m_currentBitrate). On the A->B primary edge the new primary adopts THIS, so
    // the camera stays continuous across the switch. Sentinel -1 = NEVER
    // dispatched (true startup) — the edge falls back to B's own value then. A
    // telemetry PAUSE (0) is deliberately NOT cached, so a switch during a pause
    // carries the last RUNNING value rather than an ambiguous 0 (H1).
    int _lastEffectiveKbps = -1;
    // m_switchClock / m_lastSwitchMs: monotonic clock + the ms of the last
    // SUCCESSFUL adopt (a debounced edge does NOT refresh it), for the
    // anti-jitter debounce (ABR_SWITCH_DEBOUNCE_MS). A flip-flop that reverses
    // inside the window does not re-adopt. Sentinel = -debounce (NOT 0) so the
    // first switch is never suppressed — same pattern as m_lastResetMs.
    QElapsedTimer _switchClock;
    qint64 _lastSwitchMs = -ABR_SWITCH_DEBOUNCE_MS;
};

#endif // ABRFACTORY_H
