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

    // targetBitrateKbps đã được thuật toán BelaCoder phân tích và quyết định chuẩn xác
    m_smoothedBitrate = static_cast<double>(targetBitrateKbps);

    // Xác định nấc độ phân giải mục tiêu chuẩn theo Bitrate:
    // Nới rộng dải 1080p và 720p để Encoder tự điều tiết QP (Quantization Parameter),
    // hạn chế tối đa việc đổi Resolution (vốn bắt buộc phải sinh SPS/PPS và IDR Keyframe gây rách hình dưới mạng loss)
    int targetLevel = 1;
    if (targetBitrateKbps >= 2500) {
        targetLevel = 4; // 1080p (2500 - 6000 kbps: 1080p nén mượt ở 30fps)
    } else if (targetBitrateKbps >= 1400) {
        targetLevel = 3; // 720p (1400 - 2499 kbps, bao gồm case 1800k HeavyModerate)
    } else if (targetBitrateKbps >= 800) {
        targetLevel = 2; // 480p (800 - 1399 kbps, bao gồm case 1000k HeavySevere)
    } else {
        targetLevel = 1; // 360p (< 800 kbps, sàn sinh tồn 400k - 700k)
    }

    int currentLevel = getProfileLevel(m_currentProfile);

    // 1. HẠ ĐỘ PHÂN GIẢI (Downscale):
    // Bảo vệ camera pipeline: Chặn việc đổi nấc liên tục trong thời gian ngắn (vốn gây sinh nhiều IDR Frame làm sập stream dưới 50-70% loss).
    // Phải cách lần đổi nấc trước ít nhất MIN_DOWNSCALE_COOLDOWN_MS (3 giây), trừ khi là lần đầu khởi tạo.
    if (targetLevel < currentLevel) {
        bool canDownscale = (m_lastSwitchTimeMs == 0 || (now - m_lastSwitchTimeMs >= MIN_DOWNSCALE_COOLDOWN_MS));

        if (canDownscale) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo().noquote() << QString("[Resolution] 🟡 Hạ: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
                       .arg(m_currentProfile.label)
                       .arg(m_currentProfile.width)
                       .arg(m_currentProfile.height)
                       .arg(m_currentProfile.fps)
                       .arg(targetBitrateKbps);
        }
    }
    // 2. NÂNG ĐỘ PHÂN GIẢI (Upscale):
    // Nhảy THẲNG lên targetLevel (ví dụ 360p -> 1080p) sau khi mạng Clear và Bitrate ổn định trong 2 chu kỳ (~1.6s).
    // Triệt tiêu hoàn toàn độ trễ nâng từng nấc và chỉ sinh ĐÚNG 1 KEYFRAME DUY NHẤT, giúp hình ảnh phục hồi 1080p tức thì!
    else if (targetLevel > currentLevel) {
        if (m_lastSwitchTimeMs == 0) {
            m_currentProfile = getProfileByLevel(targetLevel);
            m_lastSwitchTimeMs = now;
            m_consecutiveUpgradeCount = 0;

            qInfo().noquote() << QString("[Resolution] 🟢 Khởi động: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
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

                qInfo().noquote() << QString("[Resolution] 🟢 Nâng trực tiếp: %1 (%2x%3 @%4fps) - Bitrate: %5 kbps")
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
