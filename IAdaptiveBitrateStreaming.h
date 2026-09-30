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
    virtual bool isTrafficActive() const { return true; }

signals:
    void bitrateChanged(unsigned int new_bitrate_kbps);
    void onStatus(int status);
    void videoStreamEnableChanged(bool isEnabled);
    void c2PriorityChanged(int priorityLevel, const QString &priorityName);
    void requestKeyframe();
    void trafficActiveChanged(bool isActive);
};

#endif // IADAPTIVEBITRATESTREAMING_H
