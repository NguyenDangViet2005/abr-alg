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

#endif // ABRCONFIGS_H
