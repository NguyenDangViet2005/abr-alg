#ifndef ABRCONFIGS_H
#define ABRCONFIGS_H

// ── SRT QoS UDP listener ──
// UDP port for receiving QoS connection stats from an external XBSRTFactory.
#define SRT_ABR_QOS_UDP_PORT 12345

// ── Router telemetry UDP listener ──
// UDP port for the router's RAW per-interface metrics (id 60, ConnType rows).
// Same wire shape as XBFirmServerListener's shared telemetry port: 2-byte id,
// then one connType byte, then a '#'-separated payload.
#define ROUTER_TELEMETRY_UDP_PORT 12346

// ── CameraControl (camera adapt-bitrate API) ──
#define CAMERA_ADAPT_BITRATE_HOST "127.0.0.1"
#define CAMERA_ADAPT_BITRATE_PORT 4002
#define CAMERA_ADAPT_BITRATE_PATH "/api/camera/adapt-bitrate"

// ── ABR defaults ──
#define ABR_DEFAULT_MAX_ABR_BITRATE           6000
#define ABR_DEFAULT_ENABLE_MULTILINK          true
#define ABR_DEFAULT_C2_STRICT_VIDEO_CUTOFF    false
#define VIDEO_DISABLED_BITRATE_KBPS           0

// ── QoS debug channel (QoSDebugSender) — default OFF ──
// Dashboard's UDP port (see qos-dashboard/server.js UDP_PORT). Disabled by
// default so the rig pays no IO cost unless --qos-debug is passed.
// This host is only the FALLBACK: --qos-debug-host (main.cpp) overrides it at
// runtime. The dashboard binds 0.0.0.0:9999, so it answers on whichever box it
// runs — the host here is where that box lives, not a dashboard-side setting.
#define QOS_DEBUG_DEFAULT_HOST  "192.168.25.197"
#define QOS_DEBUG_DEFAULT_PORT  9999
#define QOS_DEBUG_DEFAULT_DIR   "/tmp/xbqos-debug"

#endif // ABRCONFIGS_H
