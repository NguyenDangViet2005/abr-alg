#ifndef IADAPTIVEBITRATESTREAMING_H
#define IADAPTIVEBITRATESTREAMING_H

#include <QObject>

class IAdaptiveBitrateStreaming : public QObject
{
    Q_OBJECT
public:
    explicit IAdaptiveBitrateStreaming(QObject *parent = nullptr);
    virtual void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void reset(int bitrateKbps) = 0;
public slots:
    virtual void handleSerialStatus(bool isConnected) = 0;
    virtual void handleSetMaxAbrBitrate(int maxBitrate) = 0;
signals:
    void bitrateChanged(unsigned int new_bitrate_kbps);
    void onStatus(int status);  // Emits StreamingQualityStatus for UX
    void requestStartConnectionStats();
};

#endif // IADAPTIVEBITRATESTREAMING_H
