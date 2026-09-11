#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
XB-QOS-SERVICE : SIMULATOR KIỂM THỬ THUẬT TOÁN ABR (SRT -> RF)
Gửi các gói tin UDP QoS vào Port 12345 để kiểm tra phản ứng của Server ABR.
==============================================================================
"""

import socket
import json
import time
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 12345
INTERVAL = 0.25  # Gửi mỗi 250ms đúng chuẩn poll rate của hệ thống

def print_banner():
    print("=" * 70)
    print("      XB-QOS-SERVICE : BỘ TEST GIẢ LẬP ĐƯỜNG TRUYỀN QoS (UDP 12345)")
    print("=" * 70)

def send_packet(sock, host, port, rtt_ms, bw_mbps, loss_total, buffer_ms, c2_rtt=0.0, c2_loss=0, c2_only=False):
    payload = {
        "timestamp": int(time.time() * 1000),
        "source": "qos_simulator",
        "metrics": {
            "rtt_ms": float(rtt_ms),
            "estimated_bandwidth_mbps": float(bw_mbps),
            "send_rate_mbps": float(bw_mbps * 0.75),
            "total_packets_lost": int(loss_total),
            "flight_size": 15,
            "recv_buffer_ms": int(buffer_ms)
        }
    }
    
    # Kèm telemetry C2 nếu có
    if c2_rtt > 0 or c2_loss > 0 or c2_only:
        payload["c2_metrics"] = {
            "rtt_ms": float(c2_rtt),
            "rtt_var_ms": float(c2_rtt * 0.2),
            "delivery_rate_mbps": 0.5,
            "retransmits": int(c2_loss),
            "tcpi_loss": int(c2_loss),
            "unacked_pkts": 12 if c2_only else 2,
            "snd_cwnd": 10,
            "min_rtt_ms": 20.0,
            "c2_only": bool(c2_only),
            "congestion_state": "C2_ONLY" if c2_only else ("Normal" if c2_rtt < 150 else "Congested")
        }

    data = json.dumps(payload).encode('utf-8')
    sock.sendto(data, (host, port))

def run_auto_suite(host, port):
    print("\n" + "=" * 75)
    print("  [+] BẮT ĐẦU KỊCH BẢN KIỂM THỬ TỰ ĐỘNG TOÀN DIỆN (10 GIAI ĐOẠN)")
    print("  Chu trình: 1080p ➔ 720p ➔ 480p ➔ 360p ➔ C2_ONLY (OFF) ➔ 360p ➔ 480p ➔ 720p ➔ 1080p")
    print("=" * 75 + "\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total_loss = 0

    scenarios = [
        {
            "name": "Giai đoạn 1: Môi trường Hoàn Hảo (Clear State)",
            "duration": 10,
            "rtt": 18.0,
            "bw": 7.5,
            "loss_inc": 0,
            "buffer": 35,
            "c2_rtt": 18.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "1080p (Full HD)",
            "expect": ">> KỲ VỌNG: Bitrate leo dốc mạnh (> 3500 kbps) ➔ Camera hiển thị 1080p (1920x1080 @ 30fps)."
        },
        {
            "name": "Giai đoạn 2: Nhiễu Sóng RF Ngẫu Nhiên (RF Noise / Hold)",
            "duration": 8,
            "rtt": 25.0,
            "bw": 6.5,
            "loss_inc": 1,
            "buffer": 55,
            "c2_rtt": 22.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "1080p / 720p (Hold)",
            "expect": ">> KỲ VỌNG: Nhận diện nhiễu vô tuyến ngẫu nhiên (Loss rải rác nhưng RTT thấp) ➔ GIỮ NGUYÊN nấc phân giải, không hạ oan."
        },
        {
            "name": "Giai đoạn 3: Băng thông hẹp vừa phải (Moderate Link)",
            "duration": 9,
            "rtt": 48.0,
            "bw": 3.2,
            "loss_inc": 1,
            "buffer": 100,
            "c2_rtt": 35.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "720p (HD)",
            "expect": ">> KỲ VỌNG: Bitrate hạ nhẹ về 2000 - 2400 kbps ➔ Camera chuyển mượt sang 720p (1280x720 @ 30fps)."
        },
        {
            "name": "Giai đoạn 4: Chớm nghẽn / Khoảng cách xa (Mild Congestion)",
            "duration": 9,
            "rtt": 90.0,
            "bw": 1.8,
            "loss_inc": 2,
            "buffer": 180,
            "c2_rtt": 55.0,
            "c2_loss": 1,
            "c2_only": False,
            "target_res": "480p (SD)",
            "expect": ">> KỲ VỌNG: RTT tăng + Loss nhẹ ➔ Bitrate giảm về 900 - 1300 kbps ➔ Camera chuyển sang 480p (854x480 @ 25fps)."
        },
        {
            "name": "Giai đoạn 5: Nghẽn mạng nặng (Heavy Congestion)",
            "duration": 9,
            "rtt": 175.0,
            "bw": 0.9,
            "loss_inc": 5,
            "buffer": 340,
            "c2_rtt": 110.0,
            "c2_loss": 4,
            "c2_only": False,
            "target_res": "360p (Low)",
            "expect": ">> KỲ VỌNG: Mạng tắc nghẽn nghiêm trọng ➔ Bitrate ép về sàn 350 - 500 kbps ➔ Camera lập tức hạ sang 360p (640x360 @ 20fps)."
        },
        {
            "name": "Giai đoạn 6: Báo động Kênh Bay C2 (C2_ONLY Drone Safety)",
            "duration": 8,
            "rtt": 180.0,
            "bw": 0.8,
            "loss_inc": 4,
            "buffer": 320,
            "c2_rtt": 320.0,
            "c2_loss": 14,
            "c2_only": True,
            "target_res": "VIDEO OFF (Màn hình đỏ C2)",
            "expect": ">> KỲ VỌNG: C2_ONLY kích hoạt ➔ TẮT 100% video stream để cứu máy bay, Web hiện màn hình cảnh báo đỏ!"
        },
        {
            "name": "Giai đoạn 7: C2 Hồi phục & Khởi động an toàn (C2 Recovery)",
            "duration": 8,
            "rtt": 35.0,
            "bw": 2.2,
            "loss_inc": 0,
            "buffer": 60,
            "c2_rtt": 20.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "360p (Safe Resume)",
            "expect": ">> KỲ VỌNG: C2 thông suốt trở lại ➔ Tự động BẬT LẠI camera ở mức sàn an toàn 360p (500 kbps)."
        },
        {
            "name": "Giai đoạn 8: Phục hồi nấc 1 (Recovery to SD)",
            "duration": 8,
            "rtt": 28.0,
            "bw": 3.8,
            "loss_inc": 0,
            "buffer": 50,
            "c2_rtt": 20.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "480p (SD)",
            "expect": ">> KỲ VỌNG: Bitrate tích lũy vượt 1000 kbps ➔ Camera nâng nấc êm ái lên 480p (854x480 @ 25fps)."
        },
        {
            "name": "Giai đoạn 9: Phục hồi nấc 2 (Recovery to HD)",
            "duration": 8,
            "rtt": 22.0,
            "bw": 5.5,
            "loss_inc": 0,
            "buffer": 45,
            "c2_rtt": 19.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "720p (HD)",
            "expect": ">> KỲ VỌNG: Bitrate vượt 2000 kbps ➔ Camera nâng nấc tiếp lên 720p (1280x720 @ 30fps)."
        },
        {
            "name": "Giai đoạn 10: Phục hồi Đỉnh cao (Full HD Reached)",
            "duration": 10,
            "rtt": 17.0,
            "bw": 8.0,
            "loss_inc": 0,
            "buffer": 35,
            "c2_rtt": 18.0,
            "c2_loss": 0,
            "c2_only": False,
            "target_res": "1080p (Full HD)",
            "expect": ">> KỲ VỌNG: Băng thông mở rộng tối đa ➔ Bitrate đạt đỉnh (> 3500 kbps), Camera trở lại 1080p (1920x1080 @ 30fps)."
        }
    ]

    for idx, sc in enumerate(scenarios, 1):
        print("-" * 75)
        print(f"[{idx:02d}/{len(scenarios)}] {sc['name']}")
        print(f"     Mục tiêu Camera : \033[1;32m{sc['target_res']}\033[0m")
        print(f"     Thông số QoS    : RTT={sc['rtt']}ms | BW={sc['bw']}Mbps | Thời gian={sc['duration']}s")
        print(f"     {sc['expect']}")
        print("-" * 75)

        start_t = time.time()
        while time.time() - start_t < sc['duration']:
            total_loss += sc['loss_inc']
            send_packet(sock, host, port, sc['rtt'], sc['bw'], total_loss, sc['buffer'], sc['c2_rtt'], sc['c2_loss'], sc.get('c2_only', False))
            
            remain = int(sc['duration'] - (time.time() - start_t))
            c2_flag = "ON" if sc.get('c2_only', False) else "OFF"
            sys.stdout.write(f"\r  ➔ Gửi: RTT={sc['rtt']}ms, BW={sc['bw']}M, Loss={total_loss}, C2_ONLY={c2_flag} | Target: [{sc['target_res']}] | Còn {remain:2d}s   ")
            sys.stdout.flush()
            time.sleep(INTERVAL)
        print("\n")

    print("=" * 75)
    print(">>> HOÀN TẤT TOÀN DIỆN BÀI TEST 10 GIAI ĐOẠN! Camera đã diễn hoạt đủ 4 nấc phân giải và C2 Safety.")
    print("=" * 75)

def run_interactive(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total_loss = 0
    print("\n[+] CHẾ ĐỘ THỦ CÔNG: Chọn trạng thái mạng để gửi liên tục")
    print("1. Mạng Rất Tốt (RTT 18ms, BW 8.0 Mbps, Loss 0)")
    print("2. Nhiễu Sóng Nhẹ (RTT 25ms, BW 6.0 Mbps, Loss thi thoảng +1)")
    print("3. Nghẽn Trung Bình (RTT 95ms, BW 3.0 Mbps, Loss +3)")
    print("4. Nghẽn Nặng (RTT 180ms, BW 1.5 Mbps, Loss +7)")
    print("5. Sập Mạng Panic (RTT 350ms, BW 0.5 Mbps, Loss +15)")
    print("6. Báo động Link Bay C2 Chậm (C2 RTT 300ms, Loss C2 nặng)")
    print("7. Chế độ C2_ONLY Khẩn Cấp (Lập tức TẮT luồng Video để cứu Drone)")
    print("8. C2 & Video Hồi Phục Tuyệt Đối (RTT 20ms, C2 RTT 20ms, BẬT lại Video)")

    choice = input("\nChọn chế độ (1-8): ").strip()
    rtt, bw, loss_step, buf, c2_rtt, c2_loss, c2_only = 20.0, 6.0, 0, 50, 20.0, 0, False
    
    if choice == "1":
        rtt, bw, loss_step, buf = 18.0, 8.0, 0, 40
    elif choice == "2":
        rtt, bw, loss_step, buf = 25.0, 6.0, 1, 60
    elif choice == "3":
        rtt, bw, loss_step, buf = 95.0, 3.0, 3, 150
    elif choice == "4":
        rtt, bw, loss_step, buf = 180.0, 1.5, 7, 260
    elif choice == "5":
        rtt, bw, loss_step, buf = 350.0, 0.5, 15, 500
    elif choice == "6":
        rtt, bw, loss_step, buf, c2_rtt, c2_loss = 25.0, 6.0, 0, 50, 300.0, 10
    elif choice == "7":
        rtt, bw, loss_step, buf, c2_rtt, c2_loss, c2_only = 25.0, 5.0, 0, 50, 300.0, 10, True
    elif choice == "8":
        rtt, bw, loss_step, buf, c2_rtt, c2_loss, c2_only = 20.0, 6.5, 0, 40, 20.0, 0, False

    sec = int(input("Gửi trong bao nhiêu giây (ví dụ 10): ").strip() or "10")
    print(f"\n>>> Bắt đầu gửi kịch bản {choice} trong {sec} giây... (Nhấn Ctrl+C để dừng)")
    
    start_t = time.time()
    while time.time() - start_t < sec:
        total_loss += loss_step
        send_packet(sock, host, port, rtt, bw, total_loss, buf, c2_rtt, c2_loss, c2_only)
        remain = int(sec - (time.time() - start_t))
        sys.stdout.write(f"\r[+] Đang gửi: RTT={rtt}ms, BW={bw}Mbps, Loss={total_loss}, C2_ONLY={c2_only} | Còn {remain}s  ")
        sys.stdout.flush()
        time.sleep(INTERVAL)
    print("\n>>> Hoàn thành!")

if __name__ == "__main__":
    print_banner()
    host_input = input(f"Nhập IP của Server XBLink (Mặc định: {DEFAULT_HOST}): ").strip()
    target_host = host_input if host_input else DEFAULT_HOST
    target_port = DEFAULT_PORT

    print(f"\n-> Server mục tiêu: {target_host}:{target_port}")
    print("1. Chạy bài Test Tự Động Toàn Diện (Khuyên dùng)")
    print("2. Chạy bài Test Thủ Công (Tùy chọn từng mức mạng)")
    
    mode = input("Chọn chế độ (1 hoặc 2, mặc định 1): ").strip() or "1"
    if mode == "2":
        run_interactive(target_host, target_port)
    else:
        run_auto_suite(target_host, target_port)
