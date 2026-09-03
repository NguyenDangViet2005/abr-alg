param(
    [string]$TargetIP = "127.0.0.1",
    [int]$TargetPort = 9876,
    [int]$IntervalMs = 20, # Gửi 50 gói / giây (20ms/gói) để stream cực mượt và realtime
    [int]$PacketSize = 1000 # 1 KB / gói
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   SRT REAL-TIME STREAM SENDER (50 pkts/sec)" -ForegroundColor Yellow
Write-Host "   Target: $($TargetIP) : $($TargetPort)" -ForegroundColor Green
Write-Host "   Nhan Ctrl+C de dung" -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Cyan

$client = New-Object System.Net.Sockets.UdpClient
$seq = 0

try {
    while ($true) {
        $seq++
        $nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

        # Tạo chuỗi JSON có seq và timestamp_ms (Đặc trưng chuẩn SRT)
        $payloadObj = @{
            seq = $seq
            timestamp_ms = $nowMs
            data = ("X" * ($PacketSize - 80))
        }
        $jsonStr = $payloadObj | ConvertTo-Json -Compress
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($jsonStr)

        $client.Send($bytes, $bytes.Length, $TargetIP, $TargetPort) | Out-Null

        if ($seq % 50 -eq 0) {
            $timeStr = (Get-Date).ToString('HH:mm:ss')
            Write-Host "[$timeStr] Stream dang chay -> Goi #$seq ($($bytes.Length) bytes) toi $($TargetIP):$($TargetPort)" -ForegroundColor DarkGray
        }

        Start-Sleep -Milliseconds $IntervalMs
    }
}
finally {
    $client.Close()
    Write-Host "Da dung Stream Sender." -ForegroundColor Yellow
}