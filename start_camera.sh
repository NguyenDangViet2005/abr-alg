#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Dọn dẹp các instance cam_server cũ để tránh chiếm giữ port 8888 và 5005
pkill -9 -f cam_server.py 2>/dev/null
sleep 0.5

echo ">>> [Camera Streamer] Starting Adaptive HTTP MJPEG Camera Server on port 8888..."
exec python3 -u cam_server.py
