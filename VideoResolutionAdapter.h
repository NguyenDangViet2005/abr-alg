#ifndef VIDEORESOLUTIONADAPTER_H
#define VIDEORESOLUTIONADAPTER_H

#include <QString>
#include <QDateTime>
#include <QDebug>

struct VideoProfile {
    int width;
    int height;
    int fps;
    int scalePercent;
    QString name;
    QString label;
};

class VideoResolutionAdapter {
public:
    VideoResolutionAdapter();

    VideoProfile updateBitrate(unsigned int targetBitrateKbps);

    static VideoProfile profile1080p();
    static VideoProfile profile720p();
    static VideoProfile profile480p();
    static VideoProfile profile360p();
    static VideoProfile profileOff() { return VideoProfile{0, 0, 0, 0, "OFF", "OFF (Stream Disabled)"}; }

private:
    VideoProfile m_currentProfile;
    double m_smoothedBitrate;
    qint64 m_lastSwitchTimeMs;
    int m_consecutiveUpgradeCount;

    static constexpr int UPSCALE_CONFIRMATION_CYCLES = 2;
    static constexpr qint64 MIN_SWITCH_COOLDOWN_MS = 2000;
    static constexpr qint64 MIN_DOWNSCALE_COOLDOWN_MS = 600;
};

#endif // VIDEORESOLUTIONADAPTER_H
