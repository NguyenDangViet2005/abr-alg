#ifndef VIDEORESOLUTIONADAPTER_H
#define VIDEORESOLUTIONADAPTER_H

#include <QString>
#include <QDateTime>
#include <QDebug>

// Cấu trúc đại diện cho 1 cấu hình độ phân giải video
struct VideoProfile {
    int width;
    int height;
    int fps;
    int scalePercent;
    QString label;

    bool operator==(const VideoProfile &other) const {
        return width == other.width && height == other.height && fps == other.fps;
    }
    bool operator!=(const VideoProfile &other) const {
        return !(*this == other);
    }
};

/**
 * @brief Bộ điều phối độ phân giải video mượt mà (Smooth Resolution Adapter)
 * Áp dụng:
 * 1. Exponential Moving Average (EMA) để làm mịn biến động bitrate
 * 2. Hysteresis Bands (Vùng đệm trễ) triệt tiêu hiện tượng dao động lật nấc
 * 3. Chống chuyển nấc đột ngột (Step-by-Step transition)
 * 4. Damping Cooldown (Khóa thời gian tối thiểu giữa các lần đổi phân giải)
 */
class VideoResolutionAdapter {
public:
    VideoResolutionAdapter();

    // Cập nhật bitrate mới và tính toán độ phân giải mượt mà
    VideoProfile updateBitrate(unsigned int targetBitrateKbps);

    // Lấy profile hiện tại
    VideoProfile currentProfile() const { return m_currentProfile; }

    // Reset về trạng thái ban đầu
    void reset();

    // Các profile chuẩn
    static VideoProfile profile1080p();
    static VideoProfile profile720p();
    static VideoProfile profile480p();
    static VideoProfile profile360p();
    static VideoProfile profileOff() { return VideoProfile{0, 0, 0, 0, "OFF (Stream Disabled)"}; }

    double smoothedBitrate() const { return m_smoothedBitrate; }

private:
    VideoProfile getRawProfile(double bitrateKbps) const;
    VideoProfile stepTransition(const VideoProfile &current, const VideoProfile &target) const;

    VideoProfile m_currentProfile;
    double m_smoothedBitrate;
    qint64 m_lastSwitchTimeMs;
    int m_consecutiveUpgradeCount;
    int m_consecutiveDowngradeCount;

    // Các tham số điều khiển độ mượt (Tuning parameters)
    static constexpr int UPSCALE_CONFIRMATION_CYCLES = 2;    // Cần ổn định 2 chu kỳ (~1.5s) trước khi nâng nấc
    static constexpr int DOWNSCALE_CONFIRMATION_CYCLES = 1;  // Hạ ngay tức thì (1 chu kỳ) để chống nghẽn và cắt trễ
    static constexpr qint64 MIN_SWITCH_COOLDOWN_MS = 1500;   // Cooldown 1.5s tránh giật lắc
};

#endif // VIDEORESOLUTIONADAPTER_H
