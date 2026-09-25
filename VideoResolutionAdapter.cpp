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
{
}

void VideoResolutionAdapter::reset()
{
    m_currentProfile = profile360p();
    m_smoothedBitrate = 1000.0;
    m_lastSwitchTimeMs = 0;
    m_consecutiveUpgradeCount = 0;
}

void VideoResolutionAdapter::resetToProfile(const VideoProfile &profile)
{
    m_currentProfile = profile;
    m_smoothedBitrate = (profile.height >= 1080) ? 3500.0 : ((profile.height >= 720) ? 2000.0 : ((profile.height >= 480) ? 1000.0 : 450.0));
    m_lastSwitchTimeMs = QDateTime::currentMSecsSinceEpoch();
    m_consecutiveUpgradeCount = 0;
}

static int determineTargetLevel(double bitrateKbps, int currentLevel) {
    if (currentLevel == 0) { // Off -> Khởi động lại
        if (bitrateKbps >= 2500) return 4;
        if (bitrateKbps >= 1400) return 3;
        if (bitrateKbps >= 800)  return 2;
        return 1;
    }

    switch (currentLevel) {
    case 4: // 1080p: Vùng trễ [2100, 2600]
        if (bitrateKbps < 750)  return 1; // Sập mạng rớt thẳng 360p
        if (bitrateKbps < 1200) return 2; // Rớt về 480p
        if (bitrateKbps < 2100) return 3; // Hạ 720p
        return 4; // Giữ 1080p
    case 3: // 720p: Vùng trễ [1200, 1500]
        if (bitrateKbps >= 2600) return 4; // Nâng 1080p
        if (bitrateKbps < 750)  return 1; // Sập mạng
        if (bitrateKbps < 1200) return 2; // Hạ 480p
        return 3; // Giữ 720p
    case 2: // 480p: Vùng trễ [650, 850]
        if (bitrateKbps >= 2600) return 4; // Nhảy vọt 1080p
        if (bitrateKbps >= 1500) return 3; // Nâng 720p
        if (bitrateKbps < 650)  return 1; // Hạ 360p
        return 2; // Giữ 480p
    case 1: // 360p
    default:
        if (bitrateKbps >= 2600) return 4;
        if (bitrateKbps >= 1500) return 3;
        if (bitrateKbps >= 850)  return 2;
        return 1; // Giữ 360p
    }
}

VideoProfile VideoResolutionAdapter::updateBitrate(unsigned int targetBitrateKbps)
{
    if (targetBitrateKbps == 0) {
        m_currentProfile = profileOff();
        m_smoothedBitrate = 0.0;
        m_lastSwitchTimeMs = 0;
        m_consecutiveUpgradeCount = 0;
        return m_currentProfile;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // EMA smoothing: làm mịn biến động bitrate đột ngột
    if (m_smoothedBitrate <= 0.0 || m_lastSwitchTimeMs == 0) {
        m_smoothedBitrate = static_cast<double>(targetBitrateKbps);
    } else {
        // Khi bitrate giảm, dùng alpha nhạy hơn (0.65) để bám sát nhịp giảm và phản ứng kịp thời với nghẽn mạng
        const double alpha = (static_cast<double>(targetBitrateKbps) < m_smoothedBitrate) ? 0.65 : 0.40;
        m_smoothedBitrate = (alpha * static_cast<double>(targetBitrateKbps)) + ((1.0 - alpha) * m_smoothedBitrate);
    }

    int currentLevel = getProfileLevel(m_currentProfile);
    int targetLevel = determineTargetLevel(m_smoothedBitrate, currentLevel);

    if (targetLevel < currentLevel) {
        bool canDownscale = (m_lastSwitchTimeMs == 0 || (now - m_lastSwitchTimeMs >= MIN_DOWNSCALE_COOLDOWN_MS));

        if (canDownscale) {
            // Hạ từng bước một (currentLevel - 1) để bảo đảm chuyển nấc mượt mà (1080p -> 720p -> 480p -> 360p)
            int nextDownLevel = currentLevel - 1;
            m_currentProfile = getProfileByLevel(nextDownLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;
        }
    }
    else if (targetLevel > currentLevel) {
        if (m_lastSwitchTimeMs == 0 || currentLevel == 0) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;
        }
        else {
            m_consecutiveUpgradeCount++;

            if (m_consecutiveUpgradeCount >= UPSCALE_CONFIRMATION_CYCLES &&
                (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS)) {

                m_currentProfile = getProfileByLevel(targetLevel);
                m_lastSwitchTimeMs = now;
                m_consecutiveUpgradeCount = 0;
            }
        }
    }
    else {
        m_consecutiveUpgradeCount = 0;
    }

    return m_currentProfile;
}
