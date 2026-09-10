#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo ">>> [Camera Streamer] Starting Adaptive HTTP MJPEG Camera Server on port 8888..."
exec python3 cam_server.py
