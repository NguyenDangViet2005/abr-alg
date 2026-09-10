#!/bin/bash
# ==============================================================================
# XB-QOS-SERVICE : Camera Streamer Script
# Tự động được gọi bởi aiCompressor khi khởi động nếu file này tồn tại.
# ==============================================================================

DEVICE="/dev/video0"
WIDTH=1280
HEIGHT=720
FPS=30
BITRATE=2000 # kbps

echo ">>> [Camera Streamer] Initializing camera from $DEVICE..."

if [ ! -e "$DEVICE" ]; then
    echo ">>> [Camera Streamer Error] Device $DEVICE not found!"
    exit 1
fi

# Tuỳ chọn: GStreamer bắn UDP/RTP H.264
# Đổi IP host=127.0.0.1 hoặc IP trạm mặt đất GCS tuỳ theo nhu cầu hệ thống của bạn
echo ">>> [Camera Streamer] Starting GStreamer H.264 video pipeline..."
exec gst-launch-1.0 -v \
    v4l2src device=$DEVICE ! \
    video/x-raw,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 ! \
    videoconvert ! \
    x264enc tune=zerolatency bitrate=$BITRATE speed-preset=ultrafast key-int-max=30 ! \
    rtph264pay config-interval=1 pt=96 ! \
    udpsink host=127.0.0.1 port=5600 sync=false
