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

void ABRFactory::stopCameraSocketAbr()
{
    qDebug() << "ABRFactory: Stop SRT ABR";

    if (_srtAdaptiveBitrateStreaming) {
        this->disconnect(this, &ABRFactory::onSrtCameraConnection, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleQosCameraConnection);
        this->disconnect(this, &ABRFactory::onC2TelemetryData, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleC2ConnectionStats);
        _srtAdaptiveBitrateStreaming->stop();
    }
}
