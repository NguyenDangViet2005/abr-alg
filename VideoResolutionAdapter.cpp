#include "VideoResolutionAdapter.h"
#include <algorithm>

VideoProfile VideoResolutionAdapter::profile1080p() {
    return VideoProfile{1920, 1080, 30, 100, "1080p", "1080p (Full HD)"};
}

VideoProfile VideoResolutionAdapter::profile720p() {
    return VideoProfile{1280, 720, 30, 67, "720p", "720p (HD)"};
}

VideoProfile VideoResolutionAdapter::profile480p() {
    return VideoProfile{848, 480, 24, 44, "480p", "480p (SD)"};
}

VideoProfile VideoResolutionAdapter::profile360p() {
    return VideoProfile{640, 360, 20, 33, "360p", "360p (Low Survival)"};
}

static int getProfileLevel(const VideoProfile &p) {
    if (p.height >= 1080) return 4;
    if (p.height >= 720)  return 3;
    if (p.height >= 480)  return 2;
    if (p.height >= 360)  return 1;
    return 0; // OFF (Stream Disabled)
}

static VideoProfile getProfileByLevel(int level) {
    switch (level) {
    case 4: return VideoResolutionAdapter::profile1080p();
    case 3: return VideoResolutionAdapter::profile720p();
    case 2: return VideoResolutionAdapter::profile480p();
    default: return VideoResolutionAdapter::profile360p();
    }
}

VideoResolutionAdapter::VideoResolutionAdapter()
    : m_currentProfile(profile360p())
    , m_smoothedBitrate(1000.0)
    , m_lastSwitchTimeMs(0)
    , m_consecutiveUpgradeCount(0)
    , m_consecutiveDowngradeCount(0)
{
}

void VideoResolutionAdapter::reset()
{
    m_currentProfile = profile360p();
    m_smoothedBitrate = 1000.0;
    m_lastSwitchTimeMs = 0;
    m_consecutiveUpgradeCount = 0;
    m_consecutiveDowngradeCount = 0;
}

void VideoResolutionAdapter::resetToProfile(const VideoProfile &profile)
{
    m_currentProfile = profile;
    m_smoothedBitrate = (profile.height >= 1080) ? 3500.0 : ((profile.height >= 720) ? 2000.0 : ((profile.height >= 480) ? 1000.0 : 450.0));
    m_lastSwitchTimeMs = QDateTime::currentMSecsSinceEpoch();
    m_consecutiveUpgradeCount = 0;
    m_consecutiveDowngradeCount = 0;
}

VideoProfile VideoResolutionAdapter::updateBitrate(unsigned int targetBitrateKbps)
{
    if (targetBitrateKbps == 0) {
        m_currentProfile = profile360p();
        m_lastSwitchTimeMs = 0;
        return m_currentProfile;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    m_smoothedBitrate = static_cast<double>(targetBitrateKbps);

    int targetLevel = 1;
    if (targetBitrateKbps >= 2500) {
        targetLevel = 4; // 1080p (2500 - 6000 kbps: 1080p compresses smoothly at 30fps)
    } else if (targetBitrateKbps >= 1400) {
        targetLevel = 3; // 720p (1400 - 2499 kbps, includes the 1800k HeavyModerate case)
    } else if (targetBitrateKbps >= 800) {
        targetLevel = 2; // 480p (800 - 1399 kbps, includes the 1000k HeavySevere case)
    } else {
        targetLevel = 1; // 360p (< 800 kbps, includes the 250k - 799k survival floor band)
    }

    int currentLevel = getProfileLevel(m_currentProfile);

    if (targetLevel < currentLevel) {
        bool canDownscale = (m_lastSwitchTimeMs == 0 || (now - m_lastSwitchTimeMs >= MIN_DOWNSCALE_COOLDOWN_MS));

        if (canDownscale) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo().noquote() << QString("[Resolution] 🟡 Downscale: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(targetBitrateKbps);
        }
    }
  
    else if (targetLevel > currentLevel) {
        if (m_lastSwitchTimeMs == 0) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo().noquote() << QString("[Resolution] 🟢 Startup: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(targetBitrateKbps);
        }
        else {
            m_consecutiveUpgradeCount++;

            if (m_consecutiveUpgradeCount >= UPSCALE_CONFIRMATION_CYCLES &&
                (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS)) {

                m_currentProfile = getProfileByLevel(targetLevel);
                m_lastSwitchTimeMs = now;
                m_consecutiveUpgradeCount = 0;

                qInfo().noquote() << QString("[Resolution] 🟢 Direct upgrade: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
                           .arg(m_currentProfile.label)
                           .arg(m_currentProfile.width)
                           .arg(m_currentProfile.height)
                           .arg(m_currentProfile.fps)
                           .arg(targetBitrateKbps);
            }
        }
    }
    else {
        m_consecutiveUpgradeCount = 0;
    }

    return m_currentProfile;
}
