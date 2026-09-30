#ifndef ABRFACTORY_H
#define ABRFACTORY_H

#include <QObject>
#include <QVector>
#include "ABRConfigs.h"
#include "SRTAdaptiveBitrateStreaming.h"
#include "SRTPeerStat.h"

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

signals:
    void onSrtCameraConnection(const QVector<SRTPeerStat> &peers);
    void onC2TelemetryData(const QVector<SRTPeerStat> &peers);

private:
    static ABRFactory* _instance;
    SRTAdaptiveBitrateStreaming* _srtAdaptiveBitrateStreaming;
};

#endif // ABRFACTORY_H
