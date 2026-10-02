#include "ABRFactory.h"
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
}

void ABRFactory::handleRouterTick(const QString &primaryInterface)
{
    qDebug().nospace() << "[ROUTER][TICK] primary=" << primaryInterface;
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
