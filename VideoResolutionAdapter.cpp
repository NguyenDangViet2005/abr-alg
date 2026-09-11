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
    : m_currentProfile(profile720p())
    , m_smoothedBitrate(2000.0)
    , m_lastSwitchTimeMs(0)
    , m_consecutiveUpgradeCount(0)
    , m_consecutiveDowngradeCount(0)
{
}

void VideoResolutionAdapter::reset()
{
    m_currentProfile = profile720p();
    m_smoothedBitrate = 2000.0;
    m_lastSwitchTimeMs = 0;
    m_consecutiveUpgradeCount = 0;
    m_consecutiveDowngradeCount = 0;
}

VideoProfile VideoResolutionAdapter::updateBitrate(unsigned int targetBitrateKbps)
{
    if (targetBitrateKbps == 0) {
        return profileOff();
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 1. Exponential Moving Average (EMA)
    // Khi tụt bitrate: Dùng alpha lớn (0.65) để hạ độ phân giải tức thì, giải cứu băng thông
    // Khi tăng bitrate: Dùng alpha nhỏ (0.30) để lọc nhiễu, nâng nấc mượt mà
    if (m_smoothedBitrate <= 0.0) {
        m_smoothedBitrate = targetBitrateKbps;
    } else {
        double alpha = (targetBitrateKbps < m_smoothedBitrate) ? 0.65 : 0.30;
        m_smoothedBitrate = m_smoothedBitrate * (1.0 - alpha) + static_cast<double>(targetBitrateKbps) * alpha;
    }

    int currentLevel = getProfileLevel(m_currentProfile);
    int candidateLevel = currentLevel;

    // 2. Xác định nấc độ phân giải mục tiêu chính xác theo Bitrate
    if (m_smoothedBitrate >= 3200.0) {
        candidateLevel = 4; // 1080p (>= 3200 kbps)
    } else if (m_smoothedBitrate >= 1600.0) {
        candidateLevel = 3; // 720p (1600 - 3200 kbps)
    } else if (m_smoothedBitrate >= 750.0) {
        candidateLevel = 2; // 480p (750 - 1600 kbps)
    } else {
        candidateLevel = 1; // 360p (< 750 kbps, ví dụ 300k - 500k)
    }

    // 3. Vùng đệm trễ (Hysteresis Deadbands) chống lật nấc liên tục tại biên
    if (currentLevel == 3 && candidateLevel == 4 && m_smoothedBitrate < 3400.0) {
        candidateLevel = 3;
    } else if (currentLevel == 4 && candidateLevel == 3 && m_smoothedBitrate >= 2800.0) {
        candidateLevel = 4;
    } else if (currentLevel == 2 && candidateLevel == 3 && m_smoothedBitrate < 1800.0) {
        candidateLevel = 2;
    } else if (currentLevel == 3 && candidateLevel == 2 && m_smoothedBitrate >= 1400.0) {
        candidateLevel = 3;
    } else if (currentLevel == 1 && candidateLevel == 2 && m_smoothedBitrate < 850.0) {
        candidateLevel = 1;
    } else if (currentLevel == 2 && candidateLevel == 1 && m_smoothedBitrate >= 650.0) {
        candidateLevel = 2;
    }

    // 4. Xử lý chuyển đổi
    if (candidateLevel < currentLevel) {
        // HẠ ĐỘ PHÂN GIẢI: Phản ứng nhanh, hạ ngay tới candidateLevel mục tiêu
        m_consecutiveDowngradeCount++;
        m_consecutiveUpgradeCount = 0;

        if (m_consecutiveDowngradeCount >= DOWNSCALE_CONFIRMATION_CYCLES &&
            (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS || m_lastSwitchTimeMs == 0)) {
            
            m_currentProfile = getProfileByLevel(candidateLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveDowngradeCount = 0;

            qInfo() << QString("[Resolution Adapter] 🟡 HẠ độ phân giải: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(m_currentProfile.scalePercent)
                       .arg(targetBitrateKbps);
        }
    }
    else if (candidateLevel > currentLevel) {
        // NÂNG ĐỘ PHÂN GIẢI: Nâng từng nấc một (+1 level) sau khi mạng ổn định
        m_consecutiveUpgradeCount++;
        m_consecutiveDowngradeCount = 0;

        if (m_consecutiveUpgradeCount >= UPSCALE_CONFIRMATION_CYCLES &&
            (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS)) {
            
            int nextLevel = currentLevel + 1;
            m_currentProfile = getProfileByLevel(nextLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo() << QString("[Resolution Adapter] 🟢 Mượt mà NÂNG độ phân giải: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(m_currentProfile.scalePercent)
                       .arg(targetBitrateKbps);
        }
    }
    else {
        m_consecutiveUpgradeCount = 0;
        m_consecutiveDowngradeCount = 0;
    }

    return m_currentProfile;
}
