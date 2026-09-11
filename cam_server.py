#!/usr/bin/env python3
"""
XB-QOS-SERVICE : Adaptive MJPEG Camera Streamer
- Serves HTTP MJPEG stream at http://0.0.0.0:8888
- Listens on UDP 127.0.0.1:5005 for real-time QoS adaptation commands from aiCompressor:
    * Dynamic Resolution Scaling (1080p, 720p, 480p, 360p)
    * Dynamic Frame Rate (FPS throttling)
    * Dynamic JPEG Compression Quality
    * C2_ONLY Video Kill / Disabled Screen
    * Real-time HUD Status Overlay
"""

import cv2
import json
import socket
import threading
import time
import numpy as np
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

# Global dynamic parameters controlled by aiCompressor via UDP 5005
g_params_lock = threading.Lock()
g_bitrate = 2000          # kbps
g_width = 1280
g_height = 720
g_fps = 30
g_scale = 100             # %
g_enabled = True          # Video Stream Enabled
g_label = "720p (HD)"
g_last_update = time.time()

# Shared latest JPEG frame for HTTP clients
g_frame_lock = threading.Lock()
g_latest_jpeg = None
g_frame_id = 0

def udp_control_listener(host="0.0.0.0", port=5005):
    """Lắng nghe lệnh điều khiển bitrate và resolution từ aiCompressor qua UDP"""
    global g_bitrate, g_width, g_height, g_fps, g_scale, g_enabled, g_label, g_last_update
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    print(f"[CamServer] UDP Control Listener running on {host}:{port}", flush=True)

    while True:
        try:
            data, _ = sock.recvfrom(2048)
            cmd = json.loads(data.decode("utf-8"))
            with g_params_lock:
                if "bitrate" in cmd: g_bitrate = int(cmd["bitrate"])
                if "width" in cmd and cmd["width"] > 0: g_width = int(cmd["width"])
                if "height" in cmd and cmd["height"] > 0: g_height = int(cmd["height"])
                if "fps" in cmd and cmd["fps"] > 0: g_fps = int(cmd["fps"])
                if "scale" in cmd: g_scale = int(cmd["scale"])
                if "enabled" in cmd: g_enabled = bool(cmd["enabled"])
                if "label" in cmd: g_label = str(cmd["label"])
                g_last_update = time.time()

            state_str = "ENABLED" if g_enabled else "DISABLED (C2_ONLY)"
            print(f"[CamServer] ➔ Adapt Update: State={state_str} | Bitrate={g_bitrate} kbps | "
                  f"Res={g_width}x{g_height} | FPS={g_fps} | {g_label}", flush=True)
        except Exception as e:
            print(f"[CamServer Error] UDP parse error: {e}", flush=True)

def get_jpeg_quality(bitrate_kbps):
    """Tính toán chất lượng nén JPEG theo mức bitrate hiện tại"""
    if bitrate_kbps >= 4000:
        return 85
    elif bitrate_kbps >= 2500:
        return 72
    elif bitrate_kbps >= 1500:
        return 58
    elif bitrate_kbps >= 800:
        return 42
    else:
        return 28

def camera_capture_loop(device="/dev/video0"):
    """Vòng lặp đọc camera, điều chỉnh scale, nén JPEG và vẽ HUD thông số"""
    global g_latest_jpeg, g_frame_id

    cap = cv2.VideoCapture(device)
    if not cap.isOpened():
        print(f"[CamServer Error] Cannot open camera device {device}!")
        print(f"[CamServer] Attempting /dev/video1 as fallback...")
        cap = cv2.VideoCapture(1)

    if not cap.isOpened():
        print("[CamServer Fatal] No camera device could be opened! Generating test pattern.")

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

    was_disabled = False
    while True:
        with g_params_lock:
            enabled = g_enabled
            bitrate = g_bitrate
            target_w = g_width
            target_h = g_height
            fps = max(5, g_fps)
            label = g_label

        frame_interval = 1.0 / fps

        if not enabled:
            was_disabled = True
            black_frame = np.zeros((360, 640, 3), dtype=np.uint8)
            cv2.rectangle(black_frame, (10, 10), (630, 350), (0, 0, 255), 3)
            cv2.putText(black_frame, "!!! C2 SAFETY PROTOCOL ACTIVE !!!", (60, 100),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2)
            cv2.putText(black_frame, "VIDEO STREAM TEMPORARILY DISABLED", (50, 160),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
            cv2.putText(black_frame, "100% BANDWIDTH ALLOCATED TO DRONE C2 LINK", (40, 210),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1)
            cv2.putText(black_frame, "Stream will automatically resume when C2 recovers.", (50, 260),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (180, 180, 180), 1)

            _, jpeg = cv2.imencode(".jpg", black_frame, [cv2.IMWRITE_JPEG_QUALITY, 40])
            with g_frame_lock:
                g_latest_jpeg = jpeg.tobytes()
                g_frame_id += 1
            time.sleep(0.2)
            continue

        if enabled and was_disabled:
            was_disabled = False
            if cap.isOpened():
                for _ in range(2):
                    cap.grab()

        start_time = time.time()
        ret, frame = cap.read() if cap.isOpened() else (False, None)
        if not ret or frame is None:
            frame = np.zeros((480, 640, 3), dtype=np.uint8)
            cv2.putText(frame, "WAITING FOR CAMERA...", (150, 240),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)

        orig_h, orig_w = frame.shape[:2]

        # Điều chỉnh kích thước khung hình chuẩn xác theo cấu hình phân giải
        if target_w > 0 and target_h > 0 and (target_w != orig_w or target_h != orig_h):
            interp = cv2.INTER_AREA if (target_w < orig_w) else cv2.INTER_LINEAR
            frame = cv2.resize(frame, (target_w, target_h), interpolation=interp)

        cur_h, cur_w = frame.shape[:2]

        overlay_text = f"QoS Bitrate: {bitrate} kbps | {label} ({cur_w}x{cur_h}) | Target FPS: {fps}"
        cv2.rectangle(frame, (0, 0), (cur_w, 32), (0, 0, 0), -1)
        cv2.putText(frame, overlay_text, (10, 22),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 128), 2)

        quality = get_jpeg_quality(bitrate)
        _, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, quality])

        with g_frame_lock:
            g_latest_jpeg = jpeg.tobytes()
            g_frame_id += 1

        elapsed = time.time() - start_time
        sleep_time = max(0.001, frame_interval - elapsed)
        time.sleep(sleep_time)

HTML_PAGE = b"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>XB-QoS Adaptive Video Stream</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: #0b0f19; color: #f8fafc; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; padding: 16px; }
  .card { background: #161e2e; border-radius: 12px; box-shadow: 0 12px 30px rgba(0,0,0,0.6); overflow: hidden; max-width: 1280px; width: 100%; border: 1px solid #273549; }
  .header { padding: 12px 20px; background: #0f172a; border-bottom: 1px solid #273549; display: flex; justify-content: space-between; align-items: center; font-size: 14px; font-weight: 600; }
  .status-dot { display: inline-block; width: 10px; height: 10px; background: #22c55e; border-radius: 50%; margin-right: 8px; box-shadow: 0 0 8px #22c55e; }
  .video-wrapper { position: relative; width: 100%; aspect-ratio: 16/9; background: #000; display: flex; align-items: center; justify-content: center; overflow: hidden; }
  .video-wrapper img { width: 100%; height: 100%; object-fit: contain; }
  .footer { padding: 12px 20px; background: #0f172a; border-top: 1px solid #273549; display: flex; justify-content: space-between; font-size: 13px; color: #94a3b8; }
</style>
</head>
<body>
  <div class="card">
    <div class="header">
      <div><span class="status-dot"></span>XB-QOS ADAPTIVE STREAM</div>
      <div id="status-hint" style="color: #22c55e; font-size: 13px;">Live MJPEG</div>
    </div>
    <div class="video-wrapper">
      <img id="stream" src="/stream" alt="Live Camera Stream" />
    </div>
    <div class="footer">
      <span>Auto-Adaptive Resolution (1080p / 720p / 480p / 360p)</span>
      <span>Direct Stream: <a href="/stream" style="color: #38bdf8; text-decoration: none;">/stream</a></span>
    </div>
  </div>
  <script>
    const img = document.getElementById('stream');
    const hint = document.getElementById('status-hint');
    let failCount = 0;
    img.onerror = () => {
      failCount++;
      hint.textContent = 'Reconnecting... (' + failCount + ')';
      hint.style.color = '#eab308';
      setTimeout(() => {
        img.src = '/stream?t=' + Date.now();
      }, 1000);
    };
    img.onload = () => {
      failCount = 0;
      hint.textContent = 'Live MJPEG';
      hint.style.color = '#22c55e';
    };
  </script>
</body>
</html>
"""

class StreamingHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(HTML_PAGE)))
            self.end_headers()
            self.wfile.write(HTML_PAGE)
        elif self.path.startswith("/stream"):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=--jpgboundary")
            self.send_header("Cache-Control", "no-cache, private")
            self.send_header("Pragma", "no-cache")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            last_id = -1
            while True:
                try:
                    with g_frame_lock:
                        fid = g_frame_id
                        jpeg_bytes = g_latest_jpeg

                    if fid != last_id and jpeg_bytes is not None:
                        last_id = fid
                        header = (
                            f"--jpgboundary\r\n"
                            f"Content-Type: image/jpeg\r\n"
                            f"Content-Length: {len(jpeg_bytes)}\r\n\r\n"
                        ).encode("ascii")
                        self.wfile.write(header)
                        self.wfile.write(jpeg_bytes)
                        self.wfile.write(b"\r\n")
                    time.sleep(0.005)
                except (BrokenPipeError, ConnectionResetError):
                    break
                except Exception:
                    break
        else:
            self.send_error(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

def main():
    udp_thread = threading.Thread(target=udp_control_listener, daemon=True)
    udp_thread.start()

    cam_thread = threading.Thread(target=camera_capture_loop, daemon=True)
    cam_thread.start()

    port = 8888
    server = ThreadedHTTPServer(("0.0.0.0", port), StreamingHandler)
    print("=" * 65, flush=True)
    print(f"  [XB-QOS-CAMERA] Web Dashboard is LIVE at http://0.0.0.0:{port}", flush=True)
    print(f"  [XB-QOS-CAMERA] MJPEG Raw Stream at http://0.0.0.0:{port}/stream", flush=True)
    print(f"  [XB-QOS-CAMERA] UDP Control Port is LISTENING on 127.0.0.1:5005", flush=True)
    print("=" * 65, flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[CamServer] Stopping server...", flush=True)
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
