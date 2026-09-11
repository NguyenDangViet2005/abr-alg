#ifndef ABRFACTORY_H
#define ABRFACTORY_H

#include <QObject>
#include "XBAdaptiveBitrateStreaming.h"
#ifdef XBFIRM
#include "GeneralSetting.h"
#include "Settings.h"
#else
#include "ABRConfigs.h"
#endif
#include "CellularPredictive.h"
#include "SRTAdaptiveBitrateStreaming.h"

class ABRFactory : public QObject
{
    Q_OBJECT
public:
    explicit ABRFactory(QObject *parent = nullptr);
    static ABRFactory* instance();
    void init();
    void processBitrateAdaptive();
    SRTAdaptiveBitrateStreaming* srtAbr() const { return _srtAdaptiveBitrateStreaming; }
public slots:
    void startCameraSocketAbr();
    void stopCameraSocketAbr();
    void resetCameraSocketAbr(int bitrateKbps);
    void handleSerialStatus(bool isConnected);

    void handleCloudBitrateChanged(unsigned int new_bitrate_kbps);
    void handleSrtBitrateChanged(unsigned int new_bitrate_kbps);
    void handleSetMaxBitrate(int maxBitrate);
    void handleC2Data(const QVariantMap &c2Stats);
signals:
    void onSrtCameraConnection(const QVariantList &clients);
    void onSrtControllingConnection(const QVariantList &clients);
    void onC2TelemetryData(const QVariantMap &c2Stats);

    void onQosCameraConnection(const QVariantList &clients);
    void onQosControllingConnection(const QVariantList &clients);
    void onAbrRequestChangeBitrateStep(int bitrateStep);
    void onVideoStreamEnableChanged(bool isEnabled);
    void onC2PriorityChanged(int priorityLevel, const QString &priorityName);
    void onTelemetrySocketConnectionStats(double rtt,
                                          double deliveryRate,
                                          double retransmits,
                                          int tcpiLoss,
                                          int tcpiSegsout,
                                          int tcpiSndCwnd,
                                          int tcpiSndMss,
                                          int tcpiLastAckRecv,
                                          int sockOutq,
                                          int sockSndBu,
                                          int tcpiMinRtts);
    void onCellularNetworkInfo(double rsrp, double rsrq);
    void onCamSockBitrateChanged(unsigned int new_bitrate_kbps);
    void onCamSrtBitrateChanged(unsigned int new_bitrate_kbps);
    void onCamSockStatus(int status);
    void onCamSrtStatus(int status);
    void setCamSockMaxBitrate(int maxbitrate);
    void onCameraSocketConnectionStats(double rtt,
                                       double deliveryRate,
                                       double retransmits,
                                       int tcpiLoss,
                                       int tcpiSegsout,
                                       int tcpiSndCwnd,
                                       int tcpiSndMss,
                                       int tcpiLastAckRecv,
                                       int sockOutq,
                                       int sockSndBu,
                                       int tcpiMinRtt);
    void requestStartCamSockConnectionStats();

private:
    static ABRFactory* _instance;
    CellularPredictive* _cellularPredictive;
    XBAdaptiveBitrateStreaming* _cameraSocketABR;
#ifdef XBFIRM
    GeneralSetting* _generalSettings;
#endif
    bool _havingSerial;

    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;

    int _currentCloudCamBitrate;
    int _currentSrtCamBitrate;
};

#endif // ABRFACTORY_H
