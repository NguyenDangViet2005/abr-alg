#ifndef ABRCONFIGS_H
#define ABRCONFIGS_H

// Standalone configs khi XBFIRM == false (không phụ thuộc xbfirm/settings)
// Migrated từ GeneralConfigs.h + defaults từ GeneralSetting.cpp

// ── GeneralConfigs subset ──
#define SERVER_UDP_WEBAPP 3010
#define ID_MODEM_DATA_ABR_CPA "49"

// ── ABR defaults (fallback khi không có GeneralSetting/Settings) ──
// Lấy từ GeneralSetting.cpp _setting->value(..., default)
#define ABR_DEFAULT_JITTER_STABLE_THRESHOLD   15.0f
#define ABR_DEFAULT_STABLE_THRESHOLD          50.0f
#define ABR_DEFAULT_STABLE_AFTER_DECREASE     20.0f
#define ABR_DEFAULT_QUEUE_DELAY_LOW_AFTER_DECREASE 20.0f
#define ABR_DEFAULT_TELEMETRY_STABLE_TIMEOUT  3000.0f
#define ABR_DEFAULT_TELEMETRY_KICK_BITRATE    500u
#define ABR_DEFAULT_BITRATE_INCREASE_COOLDOWN 2000LL
#define ABR_DEFAULT_MAX_ABR_BITRATE           6000
#define ABR_DEFAULT_ENABLE_MULTILINK          true

// ── AICompressor defaults ──
#define AI_COMPRESSOR_DEFAULT_HOST            "127.0.0.1"
#define AI_COMPRESSOR_DEFAULT_PORT            8001
#define AI_COMPRESSOR_DEFAULT_INPUT_PIPELINE  "rtsp://192.168.144.240:8554/payload"
#define AI_COMPRESSOR_DEFAULT_OUTPUT_PIPELINE "rtsp://127.0.0.1:12345/xbstream"

// ── SRT / RF QoS Server Settings (Port 12345) ──
#define QOS_SERVER_DEFAULT_HOST               "127.0.0.1"
#define QOS_SERVER_DEFAULT_PORT               12345
#define QOS_SERVER_POLL_INTERVAL_MS           250

#endif // ABRCONFIGS_H
