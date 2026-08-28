#ifndef ABRCONFIGS_H
#define ABRCONFIGS_H

// Standalone configs used when XBFIRM == false (no dependency on xbfirm/settings).
// Migrated from GeneralConfigs.h + defaults from GeneralSetting.cpp.

// ── GeneralConfigs subset ──
#define SERVER_UDP_WEBAPP 3010
#define ID_MODEM_DATA_ABR_CPA "49"

// ── SRT QoS UDP listener (app mode, XBFIRM == false) ──
// UDP port for receiving QoS connection stats from an external XBSRTFactory.
#define SRT_ABR_QOS_UDP_PORT 12345

// ── CameraControl (camera adapt-bitrate API) ──
#define CAMERA_ADAPT_BITRATE_HOST "127.0.0.1"
#define CAMERA_ADAPT_BITRATE_PORT 4002
#define CAMERA_ADAPT_BITRATE_PATH "/api/camera/adapt-bitrate"

// ── ABR defaults (fallback when GeneralSetting/Settings is unavailable) ──
// Sourced from GeneralSetting.cpp _setting->value(..., default).
#define ABR_DEFAULT_JITTER_STABLE_THRESHOLD   15.0f
#define ABR_DEFAULT_STABLE_THRESHOLD          50.0f
#define ABR_DEFAULT_STABLE_AFTER_DECREASE     20.0f
#define ABR_DEFAULT_QUEUE_DELAY_LOW_AFTER_DECREASE 20.0f
#define ABR_DEFAULT_TELEMETRY_STABLE_TIMEOUT  3000.0f
#define ABR_DEFAULT_TELEMETRY_KICK_BITRATE    500u
#define ABR_DEFAULT_BITRATE_INCREASE_COOLDOWN 2000LL
#define ABR_DEFAULT_MAX_ABR_BITRATE           1500
#define ABR_DEFAULT_ENABLE_MULTILINK          false

// ── AICompressor defaults ──
#define AI_COMPRESSOR_DEFAULT_HOST            "127.0.0.1"
#define AI_COMPRESSOR_DEFAULT_PORT            8001
#define AI_COMPRESSOR_DEFAULT_INPUT_PIPELINE  "rtsp://192.168.144.240:8554/payload"
#define AI_COMPRESSOR_DEFAULT_OUTPUT_PIPELINE "rtsp://127.0.0.1:12345/xbstream"

#endif // ABRCONFIGS_H
