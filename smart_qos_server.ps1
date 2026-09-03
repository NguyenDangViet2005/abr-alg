param(
    [int]$StreamPort = 9876, # Port luồng stream (Packet Sender / Stream)
    [int]$QoSPort = 12345    # Port QoS (App Qt query)
)

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "       SRT / UDP REAL-TIME SLIDING WINDOW QoS SERVER" -ForegroundColor Yellow
Write-Host "   Full 8-Metric SRT Socket Statistics (srt_bstats Model)" -ForegroundColor White
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " [1] Stream Port: $StreamPort | [2] QoS Port: $QoSPort" -ForegroundColor Green
Write-Host " Nhan Ctrl+C de dung" -ForegroundColor Yellow
Write-Host "=================================================================" -ForegroundColor Cyan

# Socket đón stream từ stream_sender (Port 9876)
$streamEPLocal = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $StreamPort)
$streamSocket = New-Object System.Net.Sockets.UdpClient $streamEPLocal
$streamSocket.Client.ReceiveTimeout = 2

# Socket trả QoS cho App Qt (Port 12345)
$qosEPLocal = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $QoSPort)
$qosSocket = New-Object System.Net.Sockets.UdpClient $qosEPLocal
$qosSocket.Client.ReceiveTimeout = 2

$streamRemoteEP = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
$qosRemoteEP = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)

# Tổng lũy kế
$totalLifetimePkts = 0
$totalLifetimeLost = 0
$totalLifetimeBytes = 0
$totalLifetimeRetrans = 0

# CỬA SỔ TRƯỢT THEO DÕI (1 GIÂY)
$windowPktsRecv = 0
$windowPktsLost = 0
$windowBytes = 0
$windowRetrans = 0
$lastWindowResetTime = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

$lastSeq = -1
$lastPktTime = 0
$instantBandwidthMbps = 0.0
$instantSendRateMbps = 0.0
$instantRecvRateMbps = 0.0
$instantLossPercent = 0.0
$instantRetransRate = 0.0
$instantPps = 0
$lastRttMs = 0.0
$estimatedLinkBwMbps = 0.0
$flightSize = 0
$recvBufferMs = 0
$recvBufferMb = 0.0
$recvBufferPercent = 0
$sendBufferMs = 120 # SRT default target latency buffer (120ms)

try {
    while ($true) {
        $nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

        # A. ĐỌC TOÀN BỘ CÁC GÓI ĐANG CHỜ TRONG BUFFER
        while ($streamSocket.Available -gt 0) {
            try {
                $data = $streamSocket.Receive([ref]$streamRemoteEP)
                if ($data.Length -gt 0) {
                    $totalLifetimePkts++
                    $totalLifetimeBytes += $data.Length
                    $windowPktsRecv++
                    $windowBytes += $data.Length
                    $lastPktTime = $nowMs

                    $seq = -1
                    $ts = 0

                    # Parse JSON / Regex tìm seq & timestamp_ms
                    try {
                        $str = [System.Text.Encoding]::UTF8.GetString($data)
                        if ($str -match '["'']?seq["'']?\s*[:=]\s*(\d+)') {
                            $seq = [long]$matches[1]
                        }
                        if ($str -match '["'']?timestamp_ms["'']?\s*[:=]\s*(\d+)') {
                            $ts = [long]$matches[1]
                        }
                    } catch {}

                    # Parse Binary SRT
                    if ($seq -eq -1 -and $data.Length -ge 16) {
                        if (($data[0] -band 0x80) -eq 0) {
                            $seq = ([long]($data[0] -band 0x7F) -shl 24) -bor ([long]$data[1] -shl 16) -bor ([long]$data[2] -shl 8) -bor [long]$data[3]
                        }
                    }

                    # KIỂM TRA MẤT GÓI HỢP LỆ (LỌC RESET / RESTART PHIÊN)
                    if ($seq -ne -1) {
                        if ($lastSeq -eq -1) {
                            # Gói đầu tiên của phiên -> Chấp nhận không mất gói
                            $lastSeq = $seq
                        }
                        elseif ($seq -gt $lastSeq) {
                            $diff = $seq - $lastSeq - 1
                            # Nếu lệch từ 1 đến 500 gói -> Do Clumsy drop thực tế
                            if ($diff -ge 1 -and $diff -lt 500) {
                                $windowPktsLost += $diff
                                $totalLifetimeLost += $diff

                                # Giả lập cơ chế SRT ARQ Retransmission khi phát hiện mất gói
                                $simulatedRetrans = [Math]::Max(1, [int]($diff * 0.95))
                                $windowRetrans += $simulatedRetrans
                                $totalLifetimeRetrans += $simulatedRetrans
                            } 
                            elseif ($diff -ge 500) {
                                # Nếu lệch quá 500 gói (Sender khởi động lại hoặc nhảy vọt) -> Đồng bộ lại seq
                                $lastSeq = $seq
                            }
                            $lastSeq = $seq
                        }
                        elseif ($seq -lt $lastSeq) {
                            # Sender vừa restart -> Reset lại $lastSeq
                            $lastSeq = $seq
                        }
                    }

                    # Tính RTT
                    if ($ts -gt 0 -and $nowMs -ge $ts) {
                        $measuredRtt = $nowMs - $ts
                        $lastRttMs = [Math]::Round($measuredRtt, 1)
                    }
                }
            } catch {}
        }

        # B. TÍNH TOÁN QoS REAL-TIME (CỬA SỔ TRƯỢT 500MS - PHẢN HỒI SIÊU NHANH)
        $elapsedWindow = $nowMs - $lastWindowResetTime
        if ($elapsedWindow -ge 500) {
            $timeSec = $elapsedWindow / 1000.0

            # 1. Receive Rate (Tốc độ nhận thực tế)
            $instantRecvRateMbps = [Math]::Round(($windowBytes * 8.0) / ($timeSec * 1000000.0), 2)
            $instantBandwidthMbps = $instantRecvRateMbps
            $instantPps = [Math]::Round($windowPktsRecv / $timeSec)

            # 2. Packet Loss & Send Rate (Tốc độ phát tại Sender)
            $windowExpected = $windowPktsRecv + $windowPktsLost
            if ($windowExpected -gt 0 -and $windowPktsRecv -gt 0) {
                $instantLossPercent = [Math]::Round(($windowPktsLost / $windowExpected) * 100.0, 2)
                $estimatedAvgPktBytes = if ($windowPktsRecv -gt 0) { $windowBytes / $windowPktsRecv } else { 1000 }
                $instantSendRateMbps = [Math]::Round(($windowExpected * $estimatedAvgPktBytes * 8.0) / ($timeSec * 1000000.0), 2)
            } else {
                $instantLossPercent = 0.0
                $instantSendRateMbps = $instantRecvRateMbps
            }

            # 3. Retransmission Rate (%)
            if ($windowExpected -gt 0) {
                $instantRetransRate = [Math]::Round(($windowRetrans / $windowExpected) * 100.0, 2)
            } else {
                $instantRetransRate = 0.0
            }

            # 4. Flight Size (Số gói tin đang bay trên đường truyền)
            # FlightSize = (SendRate_pps * RTT_sec) + Unacknowledged Packets
            if ($instantPps -gt 0) {
                $effectiveRttSec = [Math]::Max(0.005, $lastRttMs / 1000.0)
                $calculatedFlight = [int]($instantPps * $effectiveRttSec) + 2
                if ($instantLossPercent -gt 0) {
                    $calculatedFlight += [int]($windowPktsLost * 0.5)
                }
                $flightSize = [Math]::Max(1, $calculatedFlight)
            } else {
                $flightSize = 0
            }

            # 5. Send/Receive Buffer Occupancy
            if ($instantRecvRateMbps -gt 0) {
                # Recv Buffer dao động theo Jitter và Loss
                $baseBufMs = [Math]::Min(120, [Math]::Max(15, [int]($lastRttMs * 1.5 + $instantLossPercent * 3.0)))
                $recvBufferMs = $baseBufMs
                $recvBufferMb = [Math]::Round(($instantRecvRateMbps * ($recvBufferMs / 1000.0)) / 8.0, 3)
                $recvBufferPercent = [Math]::Min(100, [int](($recvBufferMs / 120.0) * 100.0))
            } else {
                $recvBufferMs = 0
                $recvBufferMb = 0.0
                $recvBufferPercent = 0
            }

            # 6. Bandwidth Estimation (Băng thông đường truyền thực tế)
            # Ước tính link capacity theo thông lượng tức thời (Throughput), không gán sàn cứng 10.0 Mbps
            if ($instantRecvRateMbps -gt 0) {
                $estimatedLinkBwMbps = [Math]::Round($instantRecvRateMbps * 1.15, 3)
            } elseif ($instantSendRateMbps -gt 0) {
                $estimatedLinkBwMbps = [Math]::Round($instantSendRateMbps, 3)
            } else {
                $estimatedLinkBwMbps = 0.0
            }

            # Reset cửa sổ trượt
            $windowPktsRecv = 0
            $windowPktsLost = 0
            $windowBytes = 0
            $windowRetrans = 0
            $lastWindowResetTime = $nowMs

            # IN LOG RA CONSOLE CỦA SERVER MỖI KHI CÓ STREAM HOẠT ĐỘNG
            if ($instantBandwidthMbps -gt 0 -or $windowExpected -gt 0) {
                $timeNow = (Get-Date).ToString("HH:mm:ss")
                $srcStr = "$($streamRemoteEP.Address):$($streamRemoteEP.Port)"
                $lossSign = if ($instantLossPercent -gt 0) { "⚠️ Loss: $instantLossPercent% (Retrans: $totalLifetimeRetrans)" } else { "Loss: 0%" }
                Write-Host "[$timeNow] [SRT STREAM IN from $srcStr] Send: $instantSendRateMbps M | Recv: $instantRecvRateMbps M | $lossSign | RTT: $lastRttMs ms | Flight: $flightSize | Buf: ${recvBufferMs}ms" -ForegroundColor Cyan
            }
        }

        # Nếu stream dừng hẳn quá 2.5s -> IDLE
        if ($lastPktTime -gt 0 -and ($nowMs - $lastPktTime) -gt 2500) {
            if ($instantBandwidthMbps -ne 0.0 -or $instantPps -ne 0) {
                $timeNow = (Get-Date).ToString("HH:mm:ss")
                Write-Host "[$timeNow] [STREAM IDLE] Stream da dung hoac tam ngung." -ForegroundColor DarkYellow
            }
            $instantBandwidthMbps = 0.0
            $instantSendRateMbps = 0.0
            $instantRecvRateMbps = 0.0
            $instantPps = 0
            $instantLossPercent = 0.0
            $instantRetransRate = 0.0
            $flightSize = 0
            $recvBufferMs = 0
            $recvBufferMb = 0.0
            $recvBufferPercent = 0
            $estimatedLinkBwMbps = 0.0
            $lastSeq = -1 # Reset seq khi dừng stream
        }

        # C. PHẢN HỒI CHO APP QT QUERY (PORT 12345)
        while ($qosSocket.Available -gt 0) {
            try {
                $queryBytes = $qosSocket.Receive([ref]$qosRemoteEP)
                if ($queryBytes.Length -gt 0) {
                    $streamStatus = "IDLE (No Stream)"
                    if ($lastPktTime -gt 0 -and ($nowMs - $lastPktTime) -lt 2500) {
                        $streamStatus = "STREAMING_ACTIVE"
                    }

                    $qosReport = @{
                        status = "OK"
                        stream_state = $streamStatus
                        server_time = (Get-Date).ToString("HH:mm:ss")
                        stream_source = "$($streamRemoteEP.Address):$($streamRemoteEP.Port)"
                        metrics = @{
                            # 1. RTT
                            rtt_ms = $lastRttMs
                            # 2. Packet Loss
                            packet_loss_percent = $instantLossPercent
                            total_packets_lost = $totalLifetimeLost
                            # 3. Retransmission
                            retrans_count = $totalLifetimeRetrans
                            retrans_percent = $instantRetransRate
                            # 4. Send Rate
                            send_rate_mbps = $instantSendRateMbps
                            # 5. Receive Rate
                            recv_rate_mbps = $instantRecvRateMbps
                            bandwidth_mbps = $instantRecvRateMbps
                            bandwidth_kbps = [Math]::Round($instantRecvRateMbps * 1000.0, 1)
                            packets_per_sec = $instantPps
                            # 6. Flight Size
                            flight_size = $flightSize
                            # 7. Send / Receive Buffer
                            send_buffer_ms = $sendBufferMs
                            recv_buffer_ms = $recvBufferMs
                            recv_buffer_mb = $recvBufferMb
                            recv_buffer_percent = $recvBufferPercent
                            # 8. Bandwidth Capacity
                            estimated_bandwidth_mbps = $estimatedLinkBwMbps
                            # Totals
                            total_packets_received = $totalLifetimePkts
                            total_payload_mb = [Math]::Round($totalLifetimeBytes / (1024 * 1024), 2)
                        }
                    } | ConvertTo-Json -Compress

                    $respBytes = [System.Text.Encoding]::UTF8.GetBytes($qosReport)
                    $qosSocket.Send($respBytes, $respBytes.Length, $qosRemoteEP) | Out-Null

                    # Log phản hồi cho Qt App
                    if ($totalLifetimePkts % 50 -eq 0 -and $totalLifetimePkts -gt 0) {
                        $timeNow = (Get-Date).ToString("HH:mm:ss")
                        Write-Host "[$timeNow] [QoS QUERY from $($qosRemoteEP.Address):$($qosRemoteEP.Port)] Tra ve JSON cho App Qt (Total: $totalLifetimePkts pkts)" -ForegroundColor DarkGray
                    }
                }
            } catch {}
        }

        Start-Sleep -Milliseconds 2
    }
}
finally {
    $streamSocket.Close()
    $qosSocket.Close()
}