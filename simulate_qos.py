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
    print("\n[+] BẮT ĐẦU KỊCH BẢN KIỂM THỬ TỰ ĐỘNG (7 GIAI ĐOẠN)")
    print("    Mục tiêu: Đánh giá độ nhạy, tính chuẩn xác ABR Engine & Cơ chế C2_ONLY\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total_loss = 0

    scenarios = [
        {
            "name": "Giai đoạn 1: Mạng Sạch & Lý Tưởng (Clear State)",
            "duration": 12,
            "rtt": 20.0,
            "bw": 6.5,
            "loss_inc": 0,
            "buffer": 60,
            "c2_rtt": 20.0,
            "c2_loss": 0,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: Live Bootstrap nhận diện link tốt, Bitrate tăng dần mượt mà hướng tới Max (6000 kbps)."
        },
        {
            "name": "Giai đoạn 2: Nhiễu Sóng RF Ngẫu Nhiên (RF Noise / Anti-Oscillation)",
            "duration": 10,
            "rtt": 25.0,
            "bw": 6.0,
            "loss_inc": 1,
            "buffer": 70,
            "c2_rtt": 25.0,
            "c2_loss": 0,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: Thuật toán nhận diện là NHIỄU RF (chứ không phải nghẽn) -> GIỮ NGUYÊN (HOLD) bitrate, không bị hạ oan!"
        },
        {
            "name": "Giai đoạn 3: Nghẽn Mạng Thật Sự (Congestion - RTT tăng vọt + Loss dồn)",
            "duration": 10,
            "rtt": 135.0,
            "bw": 2.2,
            "loss_inc": 5,
            "buffer": 220,
            "c2_rtt": 80.0,
            "c2_loss": 2,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: Thuật toán kích hoạt HEAVY CONGESTION -> Lập tức HẠ BITRATE (giảm 20-30%) để cứu luồng video."
        },
        {
            "name": "Giai đoạn 4: Sóng Sập Khẩn Cấp (Panic Mode)",
            "duration": 8,
            "rtt": 320.0,
            "bw": 0.8,
            "loss_inc": 12,
            "buffer": 450,
            "c2_rtt": 160.0,
            "c2_loss": 8,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: Kích hoạt PANIC MODE -> Bitrate rớt khẩn cấp về mức sàn an toàn (Min Bitrate ~ 300-500 kbps)."
        },
        {
            "name": "Giai đoạn 5: Mạng Hồi Phục (Recovery Phase)",
            "duration": 15,
            "rtt": 22.0,
            "bw": 5.5,
            "loss_inc": 0,
            "buffer": 50,
            "c2_rtt": 20.0,
            "c2_loss": 0,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: Cooldown giữ an toàn vài giây, sau đó kiểm tra Consecutive Clear và bắt đầu TĂNG LẠI từng bước."
        },
        {
            "name": "Giai đoạn 6: Thử Thách C2 Drone Priority Link (Bảo vệ Link Bay - C2 ONLY)",
            "duration": 12,
            "rtt": 25.0,
            "bw": 5.0,
            "loss_inc": 0,
            "buffer": 60,
            "c2_rtt": 280.0,
            "c2_loss": 12,
            "c2_only": True,
            "expect": ">> KỲ VỌNG: ABR nhận diện C2_ONLY -> LẬP TỨC TẮT LUỒNG VIDEO (Bitrate=0, tắt camera streamer) để nhường 100% tài nguyên cho điều khiển drone!"
        },
        {
            "name": "Giai đoạn 7: C2 Hồi Phục Bình Thường (C2 Recovery)",
            "duration": 12,
            "rtt": 22.0,
            "bw": 6.0,
            "loss_inc": 0,
            "buffer": 50,
            "c2_rtt": 20.0,
            "c2_loss": 0,
            "c2_only": False,
            "expect": ">> KỲ VỌNG: C2 bình thường trở lại -> Tự động BẬT LẠI camera stream (startCameraStreamer) và khôi phục bitrate thích ứng!"
        }
    ]

    for idx, sc in enumerate(scenarios, 1):
        print("-" * 70)
        print(f"[{idx}/{len(scenarios)}] {sc['name']}")
        print(f"    Thời gian: {sc['duration']} giây | RTT: {sc['rtt']}ms | BW: {sc['bw']}Mbps | Buffer: {sc['buffer']}ms")
        print(f"    {sc['expect']}")
        print("-" * 70)

        start_t = time.time()
        pkt_num = 0
        while time.time() - start_t < sc['duration']:
            total_loss += sc['loss_inc']
            pkt_num += 1
            send_packet(sock, host, port, sc['rtt'], sc['bw'], total_loss, sc['buffer'], sc['c2_rtt'], sc['c2_loss'], sc.get('c2_only', False))
            
            remain = int(sc['duration'] - (time.time() - start_t))
            sys.stdout.write(f"\r    -> Đang gửi: RTT={sc['rtt']}ms, BW={sc['bw']}Mbps, Loss={total_loss}, C2_ONLY={sc.get('c2_only', False)} | Còn {remain}s   ")
            sys.stdout.flush()
            time.sleep(INTERVAL)
        print("\n")

    print("=" * 70)
    print(">>> HOÀN TẤT BÀI TEST TỰ ĐỘNG! Hãy đối chiếu với log của server ./aiCompressor.")
    print("=" * 70)

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
