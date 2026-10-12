#ifndef ABRCONFIGS_H
#define ABRCONFIGS_H

// Hạ tầng (infrastructure) cho XBQoSService: port / host / path / debug.
// Các tham số THUẬT TOÁN ABR (ngưỡng, hệ số, tick) nằm ở ABRTuning.h.
#include "ABRTuning.h"

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

// ── CameraControl (xbfirm video-socket restart) ──
// xbfirm's HTTP API (SERVER_HTTP_XBFIRM = 3009) exposes /connections/video/restart,
// which drops + reopens the camera's video WebSocket — the only userspace fix for
// TCP retransmit head-of-line blocking. Same box as the adapt-bitrate API (both
// 127.0.0.1 on the QoS service host).
#define CAMERA_VIDEO_RESTART_HOST "127.0.0.1"
#define CAMERA_VIDEO_RESTART_PORT 3009
#define CAMERA_VIDEO_RESTART_PATH "/connections/video/restart"

// ── ABR aliases / SRT plumbing ──
// Router-ABR TUNING lives in ABRTuning.h (included above). These four are NOT
// router-ABR tunables: they are shared by the SRT engine + the service. Kept here
// so no SRT/dev code has to change.
//
// ABR_DEFAULT_MAX_ABR_BITRATE is now an ALIAS of the single source
// ABR_MAX_BITRATE_KBPS (ABRTuning.h) — the ceiling SHARED by both ABR engines
// (SRT/camera socket + router telemetry), so the two cannot drift. (Merging the
// former duplicate 2000 here.)
#define ABR_DEFAULT_MAX_ABR_BITRATE           ABR_MAX_BITRATE_KBPS
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
