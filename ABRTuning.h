#ifndef ABRTUNING_H
#define ABRTUNING_H

// ══════════════════════════════════════════════════════════════════════════
//  ABRTuning.h — NƠI DUY NHẤT CHỈNH THAM SỐ THUẬT TOÁN ABR (router ABR)
//
//  Tất cả "magic number" của router ABR (XBAdaptiveBitrateStreaming) nằm ở đây.
//  XBQoSService KHÔNG có file config riêng — các #define này + CLI args CHÍNH LÀ
//  config. File ABRConfigs.h giữ phần HẠ TẦNG (port/host/path); file này giữ
//  phần THUẬT TOÁN (ngưỡng, hệ số, số tick).
//
//  Cách đọc:
//    - Nhóm 1 = thông số người dùng hay chỉnh nhất → đặt trên cùng.
//    - Mỗi macro có 1 dòng comment: ý nghĩa + đơn vị + nguồn.
//    - Tag nguồn: [xbfirm] = copy verbatim từ xbfirm (bản chuẩn);
//                 [PORT DEVIATION] = thêm/sửa so với xbfirm (đã ghi rõ lý do).
//
//  LƯU Ý QUAN TRỌNG: #define là macro TOÀN CỤC (global). Vì vậy KHÔNG được khai
//  báo một class member trùng tên (preprocessor sẽ thay cả dòng khai báo → vỡ
//  compile). Mọi macro ở đây có tiền tố ABR_ để tránh trùng; các class member
//  cũ đã được xoá khỏi XBAdaptiveBitrateStreaming.
//
//  LƯU Ý VỀ KIỂU: macro ở đây KHÔNG có kiểu (mặc định int). Chỗ gọi template
//  qMin/qMax/qBound/std::min... mà vế kia là unsigned/qint64 thì deduction sẽ
//  vỡ (int vs unsigned). Phải chỉ định rõ: qMin<unsigned int>(...),
//  qBound<qint64>(...). (static constexpr cũ CÓ kiểu nên không dính lỗi này.)
// ══════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────
// NHÓM 1 — THÔNG SỐ NGƯỜI DÙNG HAY CHỈNH NHẤT
// (đổi mấy cái này là thấy khác biệt rõ nhất; sửa cẩn thận)
// ─────────────────────────────────────────────────────────────────────────

// Bitrate video tối đa (kbps) — trần tuyệt đối của mọi đề xuất bitrate.
// Đây là 1 nguồn duy nhất: ABRConfigs.h alias ABR_DEFAULT_MAX_ABR_BITRATE về đây.
// [xbfirm] = xbfirm MAX_BITRATE_KBPS (2000).
#define ABR_MAX_BITRATE_KBPS                   2000

// Bitrate video tối thiểu (kbps) — sàn; không bao giờ đề xuất dưới mức này.
// [xbfirm]
#define ABR_MIN_BITRATE_KBPS                   30

// Hệ số headroom trạng thái ổn định (%). Ramp giữ ở `pct% × trần`, chừa phần
// còn lại của link cho việc khác (C2/SRT, điều khiển, burst). 80 = dùng 80%
// trần, chừa 20%. CHỈ router ABR — engine SRT có headroom riêng (0.85).
// Override runtime: `--abr-headroom <pct>` (main.cpp). Clamp [50,100].
// [PORT DEVIATION] (xbfirm không có khái niệm trần/headroom).
#define ABR_HEADROOM_FACTOR_PCT                80

// Ngưỡng bufferbloat (ms) cho rttHigh = video (rtt − min_rtt). 500ms hàng đợi
// ĐỨNG yên đã là lag nặng; delta đã trừ độ trễ nền nên không cần "nới" thêm.
// [xbfirm] (Phase 06 từng nâng lên 700 rồi hoàn nguyên — để video lag gần 1s
// mới phanh, và lệch với cổng drain-time 400ms ở checkSenderQueue).
#define ABR_RTT_HIGH_THRESHOLD_MS              500

// Hệ số giảm "nhẹ nhàng" DUY NHẤT, dùng chung cho 3 đường giảm (loss yếu,
// rttHigh bufferbloat, qdelay tắc nghẽn) để không lệch nhau. 0.80 là hệ số
// rttHigh gốc của xbfirm. KHÁC ABR_HEADROOM_FACTOR_PCT (cùng 0.8 nhưng khác
// nghĩa: đây là bước XUỐNG khi break, kia là cap LÊN trạng thái ổn định).
// [xbfirm]
#define ABR_DECREASE_FACTOR                    0.80

// Bước tăng bitrate (kbps) mỗi lần ramp lên. [xbfirm]
#define ABR_BITRATE_STEP_KBPS                  100

// ─────────────────────────────────────────────────────────────────────────
// NHÓM 2 — NGƯỠNG THUẬT TOÁN (đơn vị ms / % / kbps / số mẫu)
// ─────────────────────────────────────────────────────────────────────────

// Ngưỡng qdelay CAO (ms): hàng đợi sender vượt mức này → coi là tắc nghẽn.
// [xbfirm]
#define ABR_QUEUE_DELAY_HIGH_THRESHOLD_MS      400.0
// Ngưỡng qdelay THẤP (ms): dưới mức này → link thông. [xbfirm]
#define ABR_QUEUE_DELAY_LOW_THRESHOLD_MS       100.0
// Sau khi giảm bitrate, qdelay phải tụt xuống dưới mức này mới coi là hết tắc.
// [xbfirm]
#define ABR_QUEUE_DELAY_LOW_AFTER_DECREASE_MS  20.0
// RTT coi là "ổn định" khi dưới mức này (ms). [xbfirm]
#define ABR_RTT_STABLE_THRESHOLD_MS            50.0
// Sau khi giảm, RTT phải xuống dưới mức này mới coi là ổn. [xbfirm]
#define ABR_RTT_STABLE_AFTER_DECREASE_MS       20.0
// Jitter coi là "ổn định" khi dưới mức này (ms). [xbfirm]
#define ABR_JITTER_STABLE_THRESHOLD_MS         15.0
// Telemetry sạch liên tục bao lâu (ms) thì khôi phục _cameraStatus = true.
// [xbfirm]
#define ABR_TELEMETRY_STABLE_TIMEOUT_MS        3000.0
// Bitrate "kick" (kbps) sau outage — đẩy 1 bước để dò lại link (post-outage
// probe, đường duy nhất được phép vượt trần). [xbfirm]
#define ABR_TELEMETRY_KICK_BITRATE_KBPS        500
// Cooldown (ms) giữa 2 lần tăng bitrate. [xbfirm]
#define ABR_BITRATE_INCREASE_COOLDOWN_MS       2000
// Số mẫu RTT giữ lại để tính jitter. [xbfirm]
#define ABR_RTT_HISTORY_SIZE                   10
// Ngưỡng loss VIDEO (%) để coi là "mất gói": vượt mức này → xét giảm.
// [xbfirm] (xbfirm dùng 0.8).
#define ABR_VIDEO_LOSS_TRIGGER_PCT             0.8
// Loss telemetry (C2) (%) vượt mức này → coi LINK yếu (không phải overshoot).
// [PORT DEVIATION] xbfirm 0.3 → 0.4: flow C2 của router nhiễu hơn probe xbfirm.
#define ABR_TELEMETRY_LOSSY_THRESHOLD_PCT      0.4
// Loss (%) coi là "sụp" nặng → đưa status về BUFFERING. [xbfirm]
#define ABR_SEVERE_LOSS_PCT                    5.0

// ─────────────────────────────────────────────────────────────────────────
// NHÓM 3 — OVERSHOOT + TRẦN (PORT DEVIATION: xbfirm không có trần)
// ─────────────────────────────────────────────────────────────────────────

// Điểm (score) tại lúc step-up phải đạt TỐI THIỂU mức này thì break mới bị coi
// là "overshoot của mình" — biên an toàn, không phải điểm tuyệt đối.
// [PORT DEVIATION]
#define ABR_OVERSHOOT_MIN_SCORE_AT_STEPUP      70.0
// Trend tại lúc step-up KHÔNG được đã đang rơi: ordinal theo ScoringModule
// (0=RAPID_INCREASE..2=STABLE, 3=DECREASE, 4=RAPID_DECREASE). <= STABLE(2).
// [PORT DEVIATION]
#define ABR_OVERSHOOT_MAX_TREND_AT_STEPUP      2
// Break phải lặp đủ số tick LIÊN TIẾP này trước khi TRẦN chốt (latch). Giảm thì
// tức thời & phục hồi được; trần thì vĩnh viễn → phải chờ xác nhận. [PORT DEVIATION]
#define ABR_OVERSHOOT_CONFIRM_TICKS            2
// Sau overshoot, HOLD (ms) và CHẤP NHẬN mất gói — loss sau step-up là hàng đợi
// đang xả, không phải break mới. [PORT DEVIATION]
#define ABR_OVERSHOOT_SETTLE_MS                3000
// Khi KHÔNG có nguồn strength: sau khi giữ ở trần bao lâu (ms) thì dò lại 1 bước.
// [PORT DEVIATION]
#define ABR_CEILING_REPROBE_BASE_MS            30000
// Trần của thời gian chờ dò lại (ms) — mỗi lần re-latch thì gấp đôi, tới mức này.
// [PORT DEVIATION]
#define ABR_CEILING_REPROBE_MAX_MS             120000

// ─────────────────────────────────────────────────────────────────────────
// NHÓM 4 — DEBOUNCE / SÀN NHIỄU (chống 1-tick spike làm sai quyết định)
// ─────────────────────────────────────────────────────────────────────────

// Cửa sổ loss VIDEO (số tick): loss% = Σlost/ΣsegsOut trên bấy nhiêu tick. Video
// chỉ ~70-80 seg/tick nên 1 gói mất đọc thành ~1.3% (>0.8% trigger) — sai. Với
// 5×70=350 seg thì 1 gói = 0.29%. VIDEO only. [PORT DEVIATION]
#define ABR_LOSS_WINDOW_TICKS                  5
// Số tick LIÊN TIẾP telemetry-lossy mới coi LINK yếu (chống C2 spike 1 tick).
// [PORT DEVIATION]
#define ABR_TELEMETRY_LOSSY_CONFIRM_TICKS      2
// Số tick LIÊN TIẾP throughput==0 mới coi là sụp thật. [PORT DEVIATION]
#define ABR_ZERO_THROUGHPUT_CONFIRM_TICKS      2
// Số điều kiện xấu liên tiếp để hạ status về POOR_NETWORK. [xbfirm]
#define ABR_POOR_CONDITIONS_TICKS              2
// Đổi bitrate phải khác ít nhất (kbps) này, nếu không thì bỏ qua (no-op). [xbfirm]
#define ABR_MIN_BITRATE_CHANGE_THRESHOLD       30
// Hệ số bitrate "kick" so với ngưỡng problematic: kick = threshold × hệ số này.
// [xbfirm]
#define ABR_TELEMETRY_KICK_FACTOR              0.9
// Không có row nào từ router trong (ms) này → ABR chạy open-loop trên input cũ,
// chỉ cảnh báo (không đổi hành vi). [PORT DEVIATION]
#define ABR_STALE_INPUT_MS                     5000

// ─────────────────────────────────────────────────────────────────────────
// NHÓM 5 — NETWORKREFERENCE (chỉ phần TINH CHỈNH; NO_INFO/0..4 là hợp đồng wire)
// ─────────────────────────────────────────────────────────────────────────

// Không có 603 trong (ms) này → coi là "không có thông tin" (fail-closed).
// [PORT DEVIATION]
#define ABR_STRENGTH_STALE_MS                  5000
// Điều kiện thô phải giữ đủ số tick TƯƠI LIÊN TIẾP này mới xác nhận. Đếm TICK
// (không phải wall-clock) để dưới im lặng thì đóng băng, không tự đủ. [PORT DEVIATION]
#define ABR_STRENGTH_CONFIRM_TICKS             5

#endif // ABRTUNING_H
