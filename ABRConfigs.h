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
// Ceiling on any proposed bitrate. SHARED by both ABR engines — the SRT/camera
// socket one (ABRFactory::startCameraSocketAbr) and the router-telemetry one
// (ABRFactory::startRouterAbr) — so this single value caps both.
//
// Set to xbfirm's MAX_BITRATE_KBPS (2000) so the router ABR's clamp matches the
// reference standard. NOTE: because the constant is shared, this also lowers the
// SRT engine's ceiling 3000 -> 2000. That engine is the one being superseded by
// the router ABR, so the lower cap is the intended direction — but it is a
// cross-engine effect, not a router-ABR-local one.
#define ABR_DEFAULT_MAX_ABR_BITRATE           2000
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
