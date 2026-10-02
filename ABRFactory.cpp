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
        << " thr="     << QString::number(row.throughputKbps, 'f', 2) << "kbps"
        << " loss="    << QString::number(row.lossRatePct, 'f', 4) << "%"
        << " rtt="     << QString::number(row.rttMs, 'f', 2) << "ms"
        << " minRtt="  << QString::number(row.minRttMs, 'f', 2) << "ms"
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
}

void ABRFactory::handleRouterAbrBitrate(const QString &iface, unsigned int kbps)
{
    const bool effective = (iface == _primaryIface);
    qInfo().nospace() << "[ABR][PROPOSE] if=" << iface << " " << kbps << "kbps "
                      << (effective ? "EFFECTIVE (primary)" : "spare");
    // v1: log only. Nothing is wired to CameraControl.
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
