#include "ABRFactory.h"
#include "XBAdaptiveBitrateStreaming.h"
#include <QDebug>

ABRFactory* ABRFactory::_instance = nullptr;

ABRFactory::ABRFactory(QObject *parent)
    : QObject(parent)
    , _srtAdaptiveBitrateStreaming(nullptr)
{
}

ABRFactory *ABRFactory::instance()
{
    if (_instance == nullptr) {
        _instance = new ABRFactory();
    }
    return _instance;
}

void ABRFactory::init()
{
}

void ABRFactory::startCameraSocketAbr()
{
    qDebug() << "ABRFactory: Start SRT ABR";

    if (_srtAdaptiveBitrateStreaming == nullptr) {
        _srtAdaptiveBitrateStreaming = new SRTAdaptiveBitrateStreaming(this);
        _srtAdaptiveBitrateStreaming->setMaxAbrBitrate(ABR_DEFAULT_MAX_ABR_BITRATE);
    }

    this->disconnect(this, &ABRFactory::onSrtCameraConnection, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleQosCameraConnection);
    this->disconnect(this, &ABRFactory::onC2TelemetryData, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleC2ConnectionStats);

    this->connect(this, &ABRFactory::onSrtCameraConnection, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleQosCameraConnection);
    this->connect(this, &ABRFactory::onC2TelemetryData, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleC2ConnectionStats);

    _srtAdaptiveBitrateStreaming->start();
}

void ABRFactory::handleC2Data(const QVector<SRTPeerStat> &peers)
{
    emit onC2TelemetryData(peers);
}

void ABRFactory::startRouterAbr(const QStringList &ifaces, bool isC2Configured)
{
    qInfo().nospace() << "ABRFactory: start router ABR for " << ifaces.size()
                      << " interface(s), isC2Configured=" << isC2Configured;

    for (const QString &name : ifaces) {
        if (_routerAbrs.contains(name)) continue;
        auto *abr = new XBAdaptiveBitrateStreaming(name, isC2Configured, this);
        abr->setMaxAbrBitrate(ABR_DEFAULT_MAX_ABR_BITRATE);
        _routerAbrs.insert(name, abr);
        connect(abr, &XBAdaptiveBitrateStreaming::bitrateProposed,
                this, &ABRFactory::handleRouterAbrBitrate);
        // Relay the full decision (with branch/reason/maxAbr) out of the
        // factory, so a tap does not need the instance map.
        connect(abr, &XBAdaptiveBitrateStreaming::decisionMade,
                this, &ABRFactory::routerAbrDecision);
    }
    qInfo().nospace() << "ABRFactory: router ABR started for " << _routerAbrs.size() << " ifaces";
}

void ABRFactory::stopRouterAbr()
{
    qDeleteAll(_routerAbrs);
    _routerAbrs.clear();
    _primaryIface.clear();
}

void ABRFactory::handleRouterRow(const RouterInterfaceStat &row)
{
    // Router's view, not XBSRTFactory's. Units are the router's own and are
    // printed rather than normalised — `lossRatePct` is a PERCENT while the SRT
    // side reports packet counts, and `throughputKbps` is kbps while the SRT
    // side reports Mbps. Normalising would hide the discrepancy this line
    // exists to expose.
    const char *label = (row.connType == ConnType::VIDEO) ? "VIDEO" : "C2   ";

    qDebug().nospace()
        << "[ROUTER] " << label
        << " if="      << row.interfaceName
        << " prio="    << row.priority
        << " act="     << row.isActivated
        << " conn="    << row.isConnected
        << " mptcp="   << row.isMptcp
        << " usable="  << row.usable
        << " score="   << QString::number(row.score, 'f', 2)
        << " trend="   << row.trend
        << " thr="     << QString::number(row.throughputKbps(), 'f', 2) << "kbps"
        << " loss="    << QString::number(row.lossRatePct, 'f', 4) << "%"
        << " rtt="     << QString::number(row.rttMs(), 'f', 2) << "ms"
        << " minRtt="  << QString::number(row.minRttMs(), 'f', 2) << "ms"
        << " qdelay="  << QString::number(row.queuingDelayMs, 'f', 2) << "ms"
        << " jitter="  << QString::number(row.jitterMs, 'f', 2) << "ms"
        << (row.parseFailed ? "  [PARSE FAILURES]" : "");

    // Route to the instance owning this interface. An unmatched name is the
    // declared-vs-seen mismatch (report risk #5) — warn, do not swallow.
    auto it = _routerAbrs.find(row.interfaceName);
    if (it == _routerAbrs.end()) {
        qWarning().nospace() << "[ROUTER] row for unknown iface '" << row.interfaceName
                             << "' — not in declared list";
        return;
    }
    (*it)->handleRouterRow(row);
}

void ABRFactory::handleRouterTick(const QString &primaryInterface)
{
    qDebug().nospace() << "[ROUTER][TICK] primary=" << primaryInterface;
    // Capture the PREVIOUS primary before overwriting, so the switch below is an
    // edge (A != B), not a level. Comparing after the assignment would always be
    // false and the forced dispatch would never fire.
    const QString prevPrimary = _primaryIface;
    _primaryIface = primaryInterface;
    // Snapshot the instances before fanning out. handleRouterTick can emit
    // requestStreamReset, and a consumer that reacts synchronously by calling
    // stopRouterAbr()/startRouterAbr() would mutate _routerAbrs mid-iteration
    // (qDeleteAll/insert during range-for = UB). Safe today only because no
    // consumer is connected yet; the copy makes it safe regardless.
    const QList<XBAdaptiveBitrateStreaming *> abrs = _routerAbrs.values();
    for (auto *abr : abrs) {
        abr->handleRouterTick(primaryInterface);
    }

    // ── Switch: force-dispatch the new primary's CURRENT bitrate ──────────
    // The per-tick proposal path is deadbanded (handleRouterTick drops an emit
    // when the value is unchanged), so a switch A→B where B already sits at its
    // value would emit NOTHING and the camera would keep A's old bitrate. This
    // is the "jump straight" the model requires: hand the camera B's value now.
    // B's value is meaningful because EVERY instance ramps in the background
    // every tick (the fan-out above), so it already reflects B's health.
    if (primaryInterface != prevPrimary) {
        auto it = _routerAbrs.find(primaryInterface);
        if (it != _routerAbrs.end()) {
            const int b = (*it)->currentBitrate();
            qInfo().nospace() << "[ABR][SWITCH] " << prevPrimary << " -> "
                              << primaryInterface << " — forcing " << b << "kbps";
            emit routerAbrEffectiveBitrate(b);
        } else {
            qWarning().nospace() << "[ABR][SWITCH] primary '" << primaryInterface
                                 << "' is not a known interface — no dispatch";
        }
    }
}

void ABRFactory::handleRouterAbrBitrate(const QString &iface, unsigned int kbps)
{
    const bool effective = (iface == _primaryIface);
    qInfo().nospace() << "[ABR][PROPOSE] if=" << iface << " " << kbps << "kbps "
                      << (effective ? "EFFECTIVE (primary)" : "spare");
    // Only the primary's proposal reaches the camera. A spare interface's
    // proposal is logged but never dispatched — acting on it would drive the
    // video with a link the router is not currently using.
    if (effective) emit routerAbrEffectiveBitrate(static_cast<int>(kbps));
}

void ABRFactory::handleCameraReportedBitrate(int achievedKbps)
{
    // Route to the primary instance only: it is the one that asked the camera
    // for a bitrate, so it is the only one whose collapse verdict is meaningful.
    auto it = _routerAbrs.find(_primaryIface);
    if (it != _routerAbrs.end()) {
        (*it)->handleCameraReportedBitrate(achievedKbps);
    }
}

void ABRFactory::stopCameraSocketAbr()
{
    qDebug() << "ABRFactory: Stop SRT ABR";

    if (_srtAdaptiveBitrateStreaming) {
        this->disconnect(this, &ABRFactory::onSrtCameraConnection, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleQosCameraConnection);
        this->disconnect(this, &ABRFactory::onC2TelemetryData, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleC2ConnectionStats);
        _srtAdaptiveBitrateStreaming->stop();
    }
}
