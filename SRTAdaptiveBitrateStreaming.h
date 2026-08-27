#ifndef SRTADAPTIVEBITRATESTREAMING_H
#define SRTADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include "IAdaptiveBitrateStreaming.h"
#include <QDebug>
#include <QVariantList>

class SRTAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    explicit SRTAdaptiveBitrateStreaming(QObject *parent = nullptr);
    void start() override;
    void stop() override;
    void reset(int bitrateKbps) override;
    void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) override;
signals:

public slots:
    void handleSerialStatus(bool isConnected) override;
    void handleSetMaxAbrBitrate(int maxBitrate) override;
    void handleQosCameraConnection(const QVariantList &clients);
    void handleQosControllingConnection(const QVariantList &clients);
};

#endif // SRTADAPTIVEBITRATESTREAMING_H
