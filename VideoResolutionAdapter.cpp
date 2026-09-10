#include "VideoResolutionAdapter.h"
#include <algorithm>

VideoProfile VideoResolutionAdapter::profile1080p() {
    return VideoProfile{1920, 1080, 30, 100, "1080p (Full HD)"};
}

VideoProfile VideoResolutionAdapter::profile720p() {
    return VideoProfile{1280, 720, 30, 75, "720p (HD)"};
}

VideoProfile VideoResolutionAdapter::profile480p() {
    return VideoProfile{854, 480, 25, 50, "480p (SD)"};
}

VideoProfile VideoResolutionAdapter::profile360p() {
    return VideoProfile{640, 360, 20, 33, "360p (Low)"};
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

    // 1. Exponential Moving Average (EMA) để lọc các xung giật bitrate nhất thời (Alpha = 0.25)
    if (m_smoothedBitrate <= 0.0) {
        m_smoothedBitrate = targetBitrateKbps;
    } else {
        m_smoothedBitrate = m_smoothedBitrate * 0.75 + static_cast<double>(targetBitrateKbps) * 0.25;
    }

    int currentLevel = getProfileLevel(m_currentProfile);
    int candidateLevel = currentLevel;

    // 2. Vùng đệm trễ (Hysteresis Bands) chống rung lắc giữa các nấc
    // Ngưỡng nâng (Upscale): Cần bitrate vượt hẳn mốc cao để chắc chắn mạng đã ổn
    if (currentLevel < 4 && m_smoothedBitrate >= 3800.0) {
        candidateLevel = 4;
    } else if (currentLevel < 3 && m_smoothedBitrate >= 2100.0) {
        candidateLevel = 3;
    } else if (currentLevel < 2 && m_smoothedBitrate >= 1100.0) {
        candidateLevel = 2;
    }

    // Ngưỡng hạ (Downscale): Có khoảng chết an toàn (Deadband) 300-600 kbps tránh hạ vội
    if (currentLevel == 4 && m_smoothedBitrate < 3200.0) {
        candidateLevel = 3;
    } else if (currentLevel >= 3 && m_smoothedBitrate < 1600.0) {
        candidateLevel = 2;
    } else if (currentLevel >= 2 && m_smoothedBitrate < 850.0) {
        candidateLevel = 1;
    }

    // 3. Xử lý logic chuyển đổi mượt mà (Smooth Stepping)
    if (candidateLevel > currentLevel) {
        m_consecutiveUpgradeCount++;
        m_consecutiveDowngradeCount = 0;

        // Cần duy trì bitrate cao liên tục và thỏa mãn thời gian cooldown
        if (m_consecutiveUpgradeCount >= UPSCALE_CONFIRMATION_CYCLES &&
            (now - m_lastSwitchTimeMs >= MIN_SWITCH_COOLDOWN_MS)) {
            
            // Nâng từng nấc một (+1 level) để chuyển tiếp mượt mà, không nhảy cóc
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
                       .arg(static_cast<int>(m_smoothedBitrate));
        }
    }
    else if (candidateLevel < currentLevel) {
        m_consecutiveDowngradeCount++;
        m_consecutiveUpgradeCount = 0;

        // Nếu mạng tụt cực sâu (< 500 kbps - tình trạng khẩn cấp) thì cho phép hạ nhanh
        bool isEmergency = (targetBitrateKbps < 500 && currentLevel > 2);
        qint64 requiredCooldown = isEmergency ? 1500 : MIN_SWITCH_COOLDOWN_MS;

        if (m_consecutiveDowngradeCount >= DOWNSCALE_CONFIRMATION_CYCLES &&
            (now - m_lastSwitchTimeMs >= requiredCooldown)) {
            
            // Hạ từng nấc một (-1 level) để mắt người xem không bị sốc hình ảnh
            int nextLevel = isEmergency ? candidateLevel : (currentLevel - 1);
            m_currentProfile = getProfileByLevel(nextLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveDowngradeCount = 0;

            qInfo() << QString("[Resolution Adapter] 🟡 Mượt mà HẠ độ phân giải: %1 (%2x%3 @ %4fps, Scale: %5%) - Bitrate: %6 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(m_currentProfile.scalePercent)
                       .arg(static_cast<int>(m_smoothedBitrate));
        }
    }
    else {
        // Trạng thái cân bằng, reset bộ đếm
        m_consecutiveUpgradeCount = 0;
        m_consecutiveDowngradeCount = 0;
    }

    return m_currentProfile;
}
