#ifndef SRTTUNING_H
#define SRTTUNING_H

#include <cstdint>

// SRT algorithm tuning (compile-time; rebuild after editing). Router tuning
// remains in ABRTuning.h. Units are included in names: kbps, ms, percent.
namespace SRTTuning {
// Startup stays conservative even when the quality ceiling is raised.
inline constexpr unsigned int STARTUP_BITRATE_KBPS = 1000;
inline constexpr unsigned int DEFAULT_MIN_BITRATE_KBPS = 0;
inline constexpr unsigned int DEFAULT_MAX_BITRATE_KBPS = 6000;
inline constexpr unsigned int DEFAULT_INITIAL_BITRATE_KBPS = STARTUP_BITRATE_KBPS;
inline constexpr unsigned int MIN_ACTIVE_VIDEO_BITRATE_KBPS = 50;

// Decision cadence and recovery ramp.
inline constexpr std::int64_t BITRATE_INCR_DECISION_INTERVAL_MS = 2000;
inline constexpr std::int64_t BITRATE_DECR_FAST_INTERVAL_MS = 250;
inline constexpr std::int64_t BITRATE_DECR_NORMAL_INTERVAL_MS = 400;
inline constexpr std::int64_t RECOVERY_COOLDOWN_MS = 3000;
inline constexpr std::int64_t CLEAR_STABLE_DURATION_MS = 1500;
inline constexpr unsigned int RF_RECOVERY_STEP_PERCENT = 50;
inline constexpr unsigned int RF_RECOVERY_MAX_STEP_KBPS = 800;
inline constexpr std::int64_t RF_RECOVERY_INTERVAL_MS = 1000;

// Measurement smoothing and bandwidth reservation.
inline constexpr int SLIDING_WINDOW_SIZE = 5;
inline constexpr double MIN_VALID_RTT_MS = 5.0;
inline constexpr double BW_UTILIZATION_RATIO = 0.90;

// Learned-ceiling probes.
inline constexpr unsigned int FAILURE_MEMORY_PROBE_STEP_KBPS = 100;
inline constexpr std::int64_t FAILURE_MEMORY_PROBE_INTERVAL_MS = 5000;
inline constexpr std::int64_t FAILURE_MEMORY_RETENTION_MS = 5000;
inline constexpr std::int64_t FAILURE_MEMORY_PROBE_CONFIRM_MS = 2000;

// RTT baseline and keyframes.
inline constexpr int RTT_BASELINE_WARMUP_SAMPLES = 5;
inline constexpr double RTT_BASELINE_DRIFT_PER_SEC = 0.002;
inline constexpr unsigned int BITRATE_ROUNDING_STEP_KBPS = 50;
inline constexpr std::int64_t KEYFRAME_REQUEST_COOLDOWN_MS = 10000;
inline constexpr std::int64_t KEYFRAME_IMMUNITY_DURATION_MS = 800;
inline constexpr std::int64_t MIN_SENT_PACKETS_FOR_LOSS_ESTIMATE = 20;

// Missing QoS / idle traffic handling.
inline constexpr std::int64_t CAMERA_QOS_STALE_TIMEOUT_MS = 5000;
inline constexpr std::int64_t CAMERA_QOS_FALLBACK_TIMEOUT_MS = 15000;
inline constexpr int CAMERA_QOS_RECOVERY_SAMPLES = 3;
inline constexpr unsigned int CAMERA_QOS_WARM_START_MIN_KBPS = 250;
inline constexpr std::int64_t TRAFFIC_IDLE_TIMEOUT_MS = 5000;

// Congestion reductions. Detailed classifier coefficients remain in the .cpp.
// LOSS_THRESHOLD_BOTTLENECK_PERCENT is a legacy constant, not a live classifier knob.
inline constexpr double LOSS_THRESHOLD_BOTTLENECK_PERCENT = 2.5;
inline constexpr double LOSS_THRESHOLD_SEVERE_PERCENT = 8.0;
inline constexpr double LOSS_THRESHOLD_PANIC_PERCENT = 25.0;
inline constexpr std::int64_t TRANSIENT_OCCLUSION_DEBOUNCE_MS = 1000;

// Recovery feedback and recent demonstrated-capacity memory.
inline constexpr std::int64_t RECOVERY_STABLE_DURATION_MS = 1500;
inline constexpr std::int64_t RECOVERY_MEMORY_LIFETIME_MS = 30000;
inline constexpr unsigned int RF_RECOVERY_MIN_STEP_KBPS = 200;
inline constexpr double RECOVERY_TARGET_LOAD_FACTOR = 0.80;
inline constexpr double RECOVERY_INCREASE_PROGRESS_FACTOR = 0.50;
inline constexpr unsigned int C2_SURVIVAL_FLOOR_KBPS = 250;
// Recovery signal gates and discovery steps.
inline constexpr double RECOVERY_MAX_RTT_RISE_MS = 2.0;
inline constexpr double RECOVERY_MAX_RAW_RTT_INFLATION_MS = 8.0;
inline constexpr double RECOVERY_MAX_SMOOTHED_RTT_INFLATION_MS = 3.0;
inline constexpr double RECOVERY_MAX_RTT_TREND_MS = 0.5;
inline constexpr unsigned int RECOVERY_FAST_HEADROOM_KBPS = 400;
inline constexpr unsigned int DISCOVERY_LARGE_HEADROOM_KBPS = 500;
inline constexpr unsigned int DISCOVERY_MEDIUM_HEADROOM_KBPS = 250;
inline constexpr unsigned int DISCOVERY_LARGE_STEP_KBPS = 200;
inline constexpr unsigned int DISCOVERY_MEDIUM_STEP_KBPS = 100;
inline constexpr std::int64_t FAILURE_MEMORY_PROBE_MAX_BACKOFF_MS = 120000;
static_assert(DEFAULT_MAX_BITRATE_KBPS >= STARTUP_BITRATE_KBPS, "SRT max must cover startup");
static_assert(DEFAULT_MAX_BITRATE_KBPS % BITRATE_ROUNDING_STEP_KBPS == 0, "SRT max must align to bitrate rounding");
} // namespace SRTTuning

// ── Bảng môi trường RF động (Duyệt động theo mảng duy nhất) ──
struct RfEnvironment {
    int minRssi;              // Ngưỡng RSSI (dBm)
    unsigned int ceilingKbps; // Trần bitrate cố định (kbps)
    const char *name;         // Tên môi trường
};

// RSSI ceilings are conservative starting points; measure on the actual link.
// Entries must remain ordered strongest to weakest with distinct ceilings.
static const RfEnvironment RF_ENVIRONMENTS[] = {
    { -62, SRTTuning::DEFAULT_MAX_BITRATE_KBPS, "Ultra" }, // >= -62 dBm
    { -69, 2500, "Strong" },                       // -69..-63 dBm
    { -74, 1500, "Mid-Range Weak" },              // -74..-70
    { -77,  800, "Far-Range" },                   // -77..-75
    { -79,  400, "Weak-Link" },                   // -79..-78
    { -86,  250, "Edge-Link" },                   // -86..-80
    { -99,  150, "Critical" }                     // <= -87
};

static const int RF_ENVIRONMENT_COUNT = sizeof(RF_ENVIRONMENTS) / sizeof(RF_ENVIRONMENTS[0]);

// Hysteresis: hạ vùng khi dưới biên; chỉ nâng vùng khi RSSI >= biên + 2 dBm.
#define RF_ENV_HYSTERESIS_DBM                 2


#endif // SRTTUNING_H
