#ifndef ABRCONFIGS_H
#define ABRCONFIGS_H

// ── SRT QoS UDP listener ──
// UDP port for receiving QoS connection stats from an external XBSRTFactory.
#define SRT_ABR_QOS_UDP_PORT 12345

// Native C2 capture (XBLink x74 / PilotApp 10.9.4). No serial/UDP ownership.
#define C2_TELEMETRY_INTERFACE                 "eth1"
// Test-mode source IP only; real mode selects local IPs of the capture interface.
// Enable with XB_C2_MODE=test. XB_C2_SOURCE_IP overrides this value.
#define C2_TELEMETRY_SOURCE_IP                 ""
#define C2_TELEMETRY_USE_BPF                   false
#define C2_TELEMETRY_UDP_PORT                  3345
#define C2_TELEMETRY_FC_SYSTEM_ID              1
#define C2_TELEMETRY_FC_COMPONENT_ID           1
#define C2_TELEMETRY_INTERVAL_MS               500
#define C2_TELEMETRY_STALE_MS                  2000
#define C2_TELEMETRY_RECOVERY_MS               3000
#define C2_HIGH_VIDEO_CEILING_KBPS             1800
#define C2_CRITICAL_VIDEO_CEILING_KBPS         200
#define C2_MIN_PACKETS_FOR_RETRANS_RATIO       20

// ── CameraControl (camera adapt-bitrate API) ──
#define CAMERA_ADAPT_BITRATE_HOST "127.0.0.1"
#define CAMERA_ADAPT_BITRATE_PORT 4002
#define CAMERA_ADAPT_BITRATE_PATH "/api/camera/adapt-bitrate"

// ── ABR defaults ──
#define ABR_DEFAULT_MAX_ABR_BITRATE           6000
#define ABR_MIN_ACTIVE_VIDEO_BITRATE_KBPS     50
#define ABR_DEFAULT_ENABLE_MULTILINK          true
#define ABR_DEFAULT_C2_STRICT_VIDEO_CUTOFF    false
#define VIDEO_DISABLED_BITRATE_KBPS           0

// ── SIYI HM30 RF Telemetry Configs ──
#define SIYI_AU_DEFAULT_IP                    "192.168.144.12"
#define SIYI_AU_DEFAULT_PORT                  19856
#define SIYI_LOCAL_DEFAULT_PORT               19857
#define SIYI_POLL_INTERVAL_MS                 500
#define SIYI_LINK_TIMEOUT_MS                  2500

// ── Bảng môi trường RF động (Duyệt động theo mảng duy nhất) ──
struct RfEnvironment {
    int minRssi;              // Ngưỡng RSSI (dBm)
    unsigned int ceilingKbps; // Trần bitrate cố định (kbps)
    const char *name;         // Tên môi trường
};

static const RfEnvironment RF_ENVIRONMENTS[] = {
    { -65, 6000, "LOS/Ultra (1080p60)" },      // Môi trường 1: Sóng cực tốt
    { -74, 3500, "Mid-Range (1080p30)" },      // Môi trường 2: Tầm trung
    { -81, 1800, "Far-Range (720p30)"  },      // Môi trường 3: Tầm xa
    { -86,  800, "Edge-Link (480p30)"  },      // Môi trường 4: Ranh giới yếu
    { -99,  200, "Critical (C2 Sống còn)"}     // Môi trường 5: Sóng cận sập
};

static const int RF_ENVIRONMENT_COUNT = sizeof(RF_ENVIRONMENTS) / sizeof(RF_ENVIRONMENTS[0]);

// Hysteresis: hạ vùng khi dưới biên; chỉ nâng vùng khi RSSI >= biên + 2 dBm.
#define RF_ENV_HYSTERESIS_DBM                 2

#endif // ABRCONFIGS_H
