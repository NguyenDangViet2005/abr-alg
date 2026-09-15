#include "ABRFactory.h"
#include <QDebug>
#include "SRTPeerStat.h"

ABRFactory* ABRFactory::_instance = nullptr;

ABRFactory::ABRFactory(QObject *parent)
    : QObject(parent)
    , _cameraSocketABR(nullptr)
    , _cellularPredictive(nullptr)
    , _havingSerial(false)
    , _srtAdaptiveBitrateStreaming(nullptr)
    , _currentCloudCamBitrate(-1)
    , _currentSrtCamBitrate(-1)
{
#ifdef XBFIRM
    _generalSettings = Settings::generalSetting();
#endif
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
    this->connect(this, SIGNAL(setCamSockMaxBitrate(int)), this, SLOT(handleSetMaxBitrate(int)));
    // this->connect(this, SIGNAL(bitrateChanged(uint)), this, SLOT(handleCloudBitrateChanged(int)));
}

void ABRFactory::processBitrateAdaptive()
{

}

void ABRFactory::startCameraSocketAbr()
{
    qDebug() << "ABRFactory: Start Cam Sock ABR";
#ifdef XBFIRM
    int maxBitrate = _generalSettings->maxAbrBitrate();
    bool enableMultilink = _generalSettings->enableMultilinkConnection();
#else
    int maxBitrate = ABR_DEFAULT_MAX_ABR_BITRATE;
    bool enableMultilink = ABR_DEFAULT_ENABLE_MULTILINK;
#endif

    if (_cameraSocketABR == nullptr) {
        _cameraSocketABR = new XBAdaptiveBitrateStreaming();
        _cameraSocketABR->setMaxAbrBitrate(maxBitrate);
    }
    if (enableMultilink && _srtAdaptiveBitrateStreaming == nullptr) {
        _srtAdaptiveBitrateStreaming = new SRTAdaptiveBitrateStreaming();
        _srtAdaptiveBitrateStreaming->setMaxAbrBitrate(maxBitrate);
    }
    if (_cellularPredictive == nullptr) {
        _cellularPredictive = new CellularPredictive();
    }
    // Connect tcpi abr
    this->disconnect(_cameraSocketABR, SIGNAL(bitrateChanged(uint)), this, SLOT(handleCloudBitrateChanged(uint)));
    this->disconnect(_cameraSocketABR, SIGNAL(onStatus(int)), this, SIGNAL(onCamSockStatus(int)));
    this->disconnect(this, SIGNAL(onCameraSocketConnectionStats(double,double,double,int,int,int,int,int,int,int,int)),
                     _cameraSocketABR, SLOT(processTCPInfo(double,double,double,int,int,int,int,int,int,int,int)));
    this->disconnect(_cameraSocketABR, SIGNAL(requestStartConnectionStats()), this, SIGNAL(requestStartCamSockConnectionStats()));
    this->disconnect(this, SIGNAL(onTelemetrySocketConnectionStats(double,double,double,int,int,int,int,int,int,int,int)),
                     _cameraSocketABR, SLOT(processTelemetryTCPInfo(double,double,double,int,int,int,int,int,int,int,int)));
    this->disconnect(this, SIGNAL(onAbrRequestChangeBitrateStep(int)), _cameraSocketABR, SLOT(handleChangeBitrateStep(int)));


    this->connect(_cameraSocketABR, SIGNAL(bitrateChanged(uint)), this, SLOT(handleCloudBitrateChanged(uint)));
    this->connect(_cameraSocketABR, SIGNAL(onStatus(int)), this, SIGNAL(onCamSockStatus(int)));
    this->connect(this, SIGNAL(onCameraSocketConnectionStats(double,double,double,int,int,int,int,int,int,int,int)),
                  _cameraSocketABR, SLOT(processTCPInfo(double,double,double,int,int,int,int,int,int,int,int)));
    this->connect(this, SIGNAL(onTelemetrySocketConnectionStats(double,double,double,int,int,int,int,int,int,int,int)),
                  _cameraSocketABR, SLOT(processTelemetryTCPInfo(double,double,double,int,int,int,int,int,int,int,int)));
    this->connect(this, SIGNAL(onAbrRequestChangeBitrateStep(int)), _cameraSocketABR, SLOT(handleChangeBitrateStep(int)));
    this->connect(_cameraSocketABR, SIGNAL(requestStartConnectionStats()), this, SIGNAL(requestStartCamSockConnectionStats()));


    this->disconnect(_cellularPredictive, SIGNAL(alphaValuesCalculated(double,double,double)), _cameraSocketABR, SLOT(updateAlphaValues(double,double,double)));
    this->disconnect(this, SIGNAL(onCellularNetworkInfo(double,double)), _cellularPredictive, SLOT(processCellularInfo(double,double)));

    this->connect(_cellularPredictive, SIGNAL(alphaValuesCalculated(double,double,double)), _cameraSocketABR, SLOT(updateAlphaValues(double,double,double)));
    this->connect(this, SIGNAL(onCellularNetworkInfo(double,double)), _cellularPredictive, SLOT(processCellularInfo(double,double)));
    // _cameraSocketABR->setMaxAbrBitrate(_generalSettings->maxAbrBitrate());
    _cameraSocketABR->handleSerialStatus(_havingSerial);
    _cameraSocketABR->start();

    if (enableMultilink) {
        qDebug() << "--> Start Qos srt camera connection";
        this->disconnect(_srtAdaptiveBitrateStreaming, &IAdaptiveBitrateStreaming::bitrateChanged, this, &ABRFactory::handleSrtBitrateChanged);
        this->disconnect(_srtAdaptiveBitrateStreaming, SIGNAL(onStatus(int)), this, SIGNAL(onCamSrtStatus(int)));
        // Lưu ý: onQosCameraConnection/onQosControllingConnection là SIGNAL của
        // ABRFactory/XBSRTFactory — KHÔNG phải của SRTAdaptiveBitrateStreaming.
        // Chuỗi QoS: XBSRTFactory::onQosCameraConnection → ABRFactory::onSrtCameraConnection
        //            → SRTAdaptiveBitrateStreaming::handleQosCameraConnection (SLOT).
        this->disconnect(this, SIGNAL(onSrtCameraConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosCameraConnection(QVector<SRTPeerStat>)));
        this->disconnect(this, SIGNAL(onSrtControllingConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosControllingConnection(QVector<SRTPeerStat>)));

        this->connect(_srtAdaptiveBitrateStreaming, &IAdaptiveBitrateStreaming::bitrateChanged, this, &ABRFactory::handleSrtBitrateChanged);
        this->connect(_srtAdaptiveBitrateStreaming, SIGNAL(onStatus(int)), this, SIGNAL(onCamSrtStatus(int)));
        this->connect(_srtAdaptiveBitrateStreaming, &IAdaptiveBitrateStreaming::videoStreamEnableChanged, this, &ABRFactory::onVideoStreamEnableChanged);
        this->connect(_srtAdaptiveBitrateStreaming, &IAdaptiveBitrateStreaming::c2PriorityChanged, this, &ABRFactory::onC2PriorityChanged);
        this->connect(this, SIGNAL(onSrtCameraConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosCameraConnection(QVector<SRTPeerStat>)));
        this->connect(this, SIGNAL(onSrtControllingConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosControllingConnection(QVector<SRTPeerStat>)));
        this->connect(this, &ABRFactory::onC2TelemetryData, _srtAdaptiveBitrateStreaming, &SRTAdaptiveBitrateStreaming::handleC2ConnectionStats);

        _srtAdaptiveBitrateStreaming->handleSerialStatus(_havingSerial);
        _srtAdaptiveBitrateStreaming->start();
    }
    // Connect srt abr
}

void ABRFactory::handleC2Data(const QVector<SRTPeerStat> &peers)
{
    if (_srtAdaptiveBitrateStreaming) {
        _srtAdaptiveBitrateStreaming->handleC2ConnectionStats(peers);
    }
    emit onC2TelemetryData(peers);
}

void ABRFactory::stopCameraSocketAbr()
{
    qDebug() << "ABRFactory: Stop Cam Sock ABR";
    if (_cameraSocketABR) {
        this->disconnect(_cameraSocketABR, SIGNAL(requestStartConnectionStats()), this, SIGNAL(requestStartCamSockConnectionStats()));
        this->disconnect(_cameraSocketABR, SIGNAL(bitrateChanged(uint)), this, SLOT(handleCloudBitrateChanged(uint)));
        this->disconnect(_cameraSocketABR, SIGNAL(onStatus(int)), this, SIGNAL(onCamSockStatus(int)));
        // this->disconnect(this, SIGNAL(setCamSockMaxBitrate(int)), _cameraSocketABR, SLOT(handleSetMaxAbrBitrate(int)));
        this->disconnect(this, SIGNAL(onAbrRequestChangeBitrateStep(int)), _cameraSocketABR, SLOT(handleChangeBitrateStep(int)));
        this->disconnect(this, SIGNAL(onCameraSocketConnectionStats(double,double,double,int,int,int,int,int,int,int,int)),
                         _cameraSocketABR, SLOT(processTCPInfo(double,double,double,int,int,int,int,int,int,int,int)));

        _cameraSocketABR->stop();
    }
    if (_cellularPredictive) {
        this->disconnect(_cellularPredictive, SIGNAL(alphaValuesCalculated(double,double,double)), _cameraSocketABR, SLOT(updateAlphaValues(double,double,double)));
        this->disconnect(this, SIGNAL(onCellularNetworkInfo(double,double)), _cellularPredictive, SLOT(processCellularInfo(double,double)));
    }

    if (_srtAdaptiveBitrateStreaming) {
        this->disconnect(_srtAdaptiveBitrateStreaming, &IAdaptiveBitrateStreaming::bitrateChanged, this, &ABRFactory::handleSrtBitrateChanged);
        this->disconnect(_srtAdaptiveBitrateStreaming, SIGNAL(onStatus(int)), this, SIGNAL(onCamSrtStatus(int)));
        this->disconnect(this, SIGNAL(onSrtCameraConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosCameraConnection(QVector<SRTPeerStat>)));
        this->disconnect(this, SIGNAL(onSrtControllingConnection(QVector<SRTPeerStat>)), _srtAdaptiveBitrateStreaming, SLOT(handleQosControllingConnection(QVector<SRTPeerStat>)));
        _srtAdaptiveBitrateStreaming->stop();
    }
}

void ABRFactory::resetCameraSocketAbr(int bitrateKbps)
{
    if (_cameraSocketABR) {
        _cameraSocketABR->reset(bitrateKbps);
    }
    if (_srtAdaptiveBitrateStreaming) {
        _srtAdaptiveBitrateStreaming->reset(bitrateKbps);
    }
}

void ABRFactory::handleSerialStatus(bool isConnected)
{
    _havingSerial = isConnected;
    if (_cameraSocketABR) {
        _cameraSocketABR->handleSerialStatus(_havingSerial);
    }
    if (_srtAdaptiveBitrateStreaming) {
        _srtAdaptiveBitrateStreaming->handleSerialStatus(_havingSerial);
    }
    qDebug() << "--> ABRFactory::handleSerialStatus: "<<isConnected;
}

void ABRFactory::handleCloudBitrateChanged(unsigned int new_bitrate_kbps)
{
    qDebug() << "--> ABRFactory::handleCloudBitrateChanged: "<<new_bitrate_kbps;
    _currentCloudCamBitrate = new_bitrate_kbps;
    emit this->onCamSockBitrateChanged(_currentCloudCamBitrate);
}

void ABRFactory::handleSrtBitrateChanged(unsigned int new_bitrate_kbps)
{
    _currentSrtCamBitrate = new_bitrate_kbps;
    emit this->onCamSrtBitrateChanged(_currentSrtCamBitrate);
    emit this->onCamSockBitrateChanged(_currentSrtCamBitrate);
}

void ABRFactory::handleSetMaxBitrate(int maxBitrate)
{
    if (_cameraSocketABR) {
        _cameraSocketABR->handleSetMaxAbrBitrate(maxBitrate);
    }
#ifdef XBFIRM
    bool enableMultilink = _generalSettings->enableMultilinkConnection();
#else
    bool enableMultilink = ABR_DEFAULT_ENABLE_MULTILINK;
#endif
    if (enableMultilink && _srtAdaptiveBitrateStreaming) {
        _srtAdaptiveBitrateStreaming->handleSetMaxAbrBitrate(maxBitrate);
    }
}


