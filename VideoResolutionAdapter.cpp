#include "VideoResolutionAdapter.h"
#include <algorithm>

VideoProfile VideoResolutionAdapter::profile1080p() {
    return VideoProfile{1920, 1080, 30, 100, "1080p (Full HD)"};
}

VideoProfile VideoResolutionAdapter::profile720p() {
    return VideoProfile{1280, 720, 30, 100, "720p (HD)"};
}

VideoProfile VideoResolutionAdapter::profile480p() {
    return VideoProfile{854, 480, 25, 67, "480p (SD)"};
}

VideoProfile VideoResolutionAdapter::profile360p() {
    return VideoProfile{640, 360, 20, 50, "360p (Low)"};
}

static int getProfileLevel(const VideoProfile &p) {
    if (p.height >= 1080) return 4;
    if (p.height >= 720)  return 3;
    if (p.height >= 480)  return 2;
    return 1; // 360p
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
        return profileOff();
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // targetBitrateKbps đã được thuật toán BelaCoder phân tích và quyết định chuẩn xác
    m_smoothedBitrate = static_cast<double>(targetBitrateKbps);

    // Xác định nấc độ phân giải mục tiêu chuẩn theo Bitrate
    int targetLevel = 1;
    if (targetBitrateKbps >= 3200) {
        targetLevel = 4; // 1080p (>= 3200 kbps)
    } else if (targetBitrateKbps >= 1600) {
        targetLevel = 3; // 720p (1600 - 3200 kbps)
    } else if (targetBitrateKbps >= 750) {
        targetLevel = 2; // 480p (750 - 1600 kbps)
    } else {
        targetLevel = 1; // 360p (< 750 kbps, ví dụ 300k - 500k)
    }

    int currentLevel = getProfileLevel(m_currentProfile);

    // 1. HẠ ĐỘ PHÂN GIẢI (Downscale):
    // Phản ứng khẩn cấp tức thì: Khi mạng tụt áp (như 400 kbps), lập tức hạ ngay về targetLevel
    // để tránh tràn hàng đợi mạng, chống lag và cắt trễ triệt để
    if (targetLevel < currentLevel) {
        m_currentProfile = getProfileByLevel(targetLevel);
        m_lastSwitchTimeMs = now;
        m_consecutiveUpgradeCount = 0;

        qInfo().noquote() << QString("[Resolution Adapter] 🟡 HẠ độ phân giải tức thì: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                   .arg(m_currentProfile.label)
                   .arg(m_currentProfile.width)
                   .arg(m_currentProfile.height)
                   .arg(m_currentProfile.fps)
                   .arg(m_currentProfile.scalePercent)
                   .arg(targetBitrateKbps);
    }
    // 2. NÂNG ĐỘ PHÂN GIẢI (Upscale):
    else if (targetLevel > currentLevel) {
        // Nếu vừa khởi động (chưa từng switch) hoặc vừa từ C2_ONLY (level 1 sau khi tắt video), cho phép nhảy thẳng đến targetLevel đo được
        if (m_lastSwitchTimeMs == 0) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo().noquote() << QString("[Resolution Adapter] 🟢 KHỞI ĐỘNG độ phân giải mục tiêu: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(m_currentProfile.scalePercent)
                       .arg(targetBitrateKbps);
        }
        else {
            m_consecutiveUpgradeCount++;

            if (m_consecutiveUpgradeCount >= UPSCALE_CONFIRMATION_CYCLES &&
                (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS)) {

                int nextLevel = currentLevel + 1;
                m_currentProfile = getProfileByLevel(nextLevel);
                m_lastSwitchTimeMs = now;
                m_consecutiveUpgradeCount = 0;

                qInfo().noquote() << QString("[Resolution Adapter] 🟢 Mượt mà NÂNG độ phân giải: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                           .arg(m_currentProfile.label)
                           .arg(m_currentProfile.width)
                           .arg(m_currentProfile.height)
                           .arg(m_currentProfile.fps)
                           .arg(m_currentProfile.scalePercent)
                           .arg(targetBitrateKbps);
            }
        }
    }
    else {
        m_consecutiveUpgradeCount = 0;
    }

    return m_currentProfile;
}
