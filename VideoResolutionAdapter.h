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

    bool operator==(const VideoProfile &other) const {
        return width == other.width && height == other.height && fps == other.fps;
    }
    bool operator!=(const VideoProfile &other) const {
        return !(*this == other);
    }
};

class VideoResolutionAdapter {
public:
    VideoResolutionAdapter();

    VideoProfile updateBitrate(unsigned int targetBitrateKbps);

    VideoProfile currentProfile() const { return m_currentProfile; }

    void reset();
    void resetToProfile(const VideoProfile &profile);

    static VideoProfile profile1080p();
    static VideoProfile profile720p();
    static VideoProfile profile480p();
    static VideoProfile profile360p();
    static VideoProfile profileOff() { return VideoProfile{0, 0, 0, 0, "OFF", "OFF (Stream Disabled)"}; }

    double smoothedBitrate() const { return m_smoothedBitrate; }

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
