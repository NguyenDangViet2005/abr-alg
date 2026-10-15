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

// Bitrate KHỞI ĐỘNG (kbps) — giá trị instance nhận ngay khi tạo (ctor) VÀ khi
// start()/reset(). Router ABR được dispatch tới camera ngay tick đầu (switch
// edge "" -> primary), nên đây CHÍNH LÀ bitrate camera thấy lúc vào, không chờ
// ramp. 1000 = vào thẳng 1000 thay vì 800 rồi bò lên (+100/2s).
// [PORT DEVIATION] (xbfirm ctor = 800, start() = 1000 — lệch nhau; đây gộp về 1).
#define ABR_STARTUP_BITRATE_KBPS               1000

// Hệ số headroom trạng thái ổn định (%) — MỨC MẶC ĐỊNH (link khỏe). Ramp giữ ở
// `pct% × trần`, chừa phần còn lại của link cho việc khác (C2/SRT, điều khiển,
// burst). 80 = dùng 80% trần, chừa 20%. Khi trần thấp (link yếu) hệ số bị THAY
// bởi 2 tầng bên dưới — xem ABR_LOW/CRITICAL_CEILING_*. CHỈ router ABR — engine
// SRT có headroom riêng (0.85). Override runtime: `--abr-headroom <pct>`
// (main.cpp). Clamp [50,100].
// [PORT DEVIATION] (xbfirm không có khái niệm trần/headroom).
#define ABR_HEADROOM_FACTOR_PCT                80

// ── Headroom 2 TẦNG theo trần (khi link yếu thì chừa nhiều hơn) ──────────
// Trần càng thấp → chừa càng nhiều theo TỈ LỆ (link yếu cần dư địa lớn hơn để
// hấp thụ burst). Ngưỡng so trên CEILING = min(m_ceilingKbps, m_maxAbr) — KHÔNG
// phải cap (cap lại phụ thuộc hệ số này → vòng lặp). Thứ tự xét: <500 → <1000 →
// mặc định. Trần < 1000 → 60%; trần < 500 → 50%. [PORT DEVIATION]
#define ABR_LOW_CEILING_THRESHOLD_KBPS         1000
#define ABR_LOW_CEILING_HEADROOM_FACTOR        0.60
#define ABR_CRITICAL_CEILING_THRESHOLD_KBPS     500
#define ABR_CRITICAL_CEILING_HEADROOM_FACTOR   0.50

// Ngưỡng bufferbloat (ms) cho rttHigh = video (rtt − min_rtt). 500ms hàng đợi
// ĐỨNG yên đã là lag nặng; delta đã trừ độ trễ nền nên không cần "nới" thêm.
// [xbfirm] (Phase 06 từng nâng lên 700 rồi hoàn nguyên — để video lag gần 1s
// mới phanh, và lệch với cổng drain-time ở checkSenderQueue: nay 200ms sau
// [PORT DEVIATION 2026-10-12]).
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
// [PORT DEVIATION 2026-10-12] 400 → 200: field test cho thấy drain-time ~150ms
// đã gây đơ/giật (drain-time là proxy độ trễ hàng đợi gửi, không phải RTT−minRtt).
// 400 để lọt vùng 150–400 vào nhánh "moderate" (chỉ HOLD, không giảm). Hạ về 200
// để phanh sớm; giữ LOW=100 nên đệm HIGH−LOW = 100ms (chống rung giảm↔tăng).
#define ABR_QUEUE_DELAY_HIGH_THRESHOLD_MS      200.0
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
// [PORT DEVIATION 2026-10-12] 0.8 → 0.5: phản ứng sớm hơn (~2 gói/5-tick thay
// vì ~3). Vẫn trên sàn nhiễu 1 gói = 1/350 ≈ 0.29% (ABR_LOSS_WINDOW_TICKS=5).
// Đây là biên "clean" của ladder (ABRLossBand.h) VÀ cổng vào haveLostPackets.
#define ABR_VIDEO_LOSS_TRIGGER_PCT             0.5
// Loss (%) coi là "sụp" nặng → đưa status về BUFFERING. [xbfirm]
#define ABR_SEVERE_LOSS_PCT                    5.0

// ── Phase 11: LOSS LADDER (PORT DEVIATION) ───────────────────────────────
// Video loss% -> bước giảm (bảng dải; logic thuần ở ABRLossBand.h):
//   <= 0.5      sạch (ABR_VIDEO_LOSS_TRIGGER_PCT; không đổi)
//   0.5 .. 10   current - ABR_LOSS_MILD_STEP_KBPS (sàn MIN)
//   10 .. 25    current * ABR_LOSS_HALVE_FACTOR   (sàn MIN)
//   25 .. 45    MIN
//   > 45        MIN + requestStreamReset (rate-limit ABR_RESET_RATE_LIMIT_MS)
// [PORT DEVIATION 2026-10-12] biên siết lại từ 0.8/20/50/70: 10% loss đã nát
// hình, dải "nhẹ" 20% cũ quá rộng. Trigger 0.8→0.5 (~2 gói/5-tick) vẫn trên
// sàn nhiễu 0.29%.
// Bước giảm "nhẹ" (kbps) cho dải 0.5..10%. Trừ THẲNG, không theo tỉ lệ — ở
// bitrate thấp (<= 230) sẽ chạm sàn MIN; chấp nhận theo luật người dùng.
#define ABR_LOSS_MILD_STEP_KBPS                200
// Trần dải "nhẹ" (%). loss > mức này -> giảm 50%.
#define ABR_LOSS_MILD_MAX_PCT                  10
// Trần dải "giảm 50%" (%). loss > mức này -> về MIN.
#define ABR_LOSS_HALVE_MAX_PCT                 25
// Hệ số giảm dải giữa. 0.50 = giảm một nửa.
#define ABR_LOSS_HALVE_FACTOR                  0.50
// Ngưỡng reset video socket (%). loss > mức này -> MIN + requestStreamReset;
// loss trong (HALVE_MAX, mức này] -> MIN (không reset). Đây là MỘT biên duy
// nhất — helper (ABRLossBand.h) lẫn log ở caller đều dùng macro này, nên không
// thể trôi nghĩa (không tách thành 2 knob cùng giá trị).
#define ABR_LOSS_RESET_PCT                     45
// Sau mỗi bước giảm của ladder, HOLD (ms) trước khi cho giảm tiếp (chờ link hồi).
#define ABR_LOSS_DECREASE_SETTLE_MS            3000
// Rate-limit (ms) giữa 2 lần emit requestStreamReset (chống loss dao động quanh
// ngưỡng reset — nay 45% — spam refresh camera). Đặt ở site emit để tín hiệu tự nó đã bị chặn.
// Dùng CHUNG cho cả đường score-cliff bên dưới (m_lastResetMs là 1 token duy nhất).
#define ABR_RESET_RATE_LIMIT_MS                30000

// ── Score-cliff -> TCP socket restart (PORT DEVIATION, Phase 12) ─────────
// Khi score VIDEO của router (row [17], 0..100) TỤT xuống dưới ĐỈNH GẦN ĐÂY một
// khoảng >= ABR_SCORE_CLIFF_DELTA thì coi là "stream kẹt" (TCP retransmit
// head-of-line blocking: gói cũ gửi mãi, chặn realtime) -> xin restart socket
// video. Đây là fix DUY NHẤT từ userspace cho ca đó — hạ bitrate KHÔNG rút được
// gói ĐÃ GỬI khỏi retransmit queue.
// Đỉnh nền = max score trong ABR_SCORE_CLIFF_WINDOW_TICKS tick gần nhất (chống
// 1 tick nhiễu đơn lẻ làm đỉnh nền tụt). Toán thuần ở ABRScoreCliff.h.
#define ABR_SCORE_CLIFF_DELTA                  50.0
// Số mẫu score VIDEO giữ lại để tính đỉnh nền (max).
#define ABR_SCORE_CLIFF_WINDOW_TICKS           5
// Số tick LIÊN TIẾP phải "cliff" trước khi fire. Score lên dây ĐÃ là EMA(α=0.5)
// nên 1 tick nhiễu không đủ sập; nhưng nhiễu dai 2-3 tick liên tiếp thì đủ. Đòi
// 2 tick liên tiếp chặn nhiễu ngắn mà vẫn bắt được sập thật (vốn dai dẳng).
// Cùng tinh thần ABR_OVERSHOOT_CONFIRM_TICKS.
#define ABR_SCORE_CLIFF_CONFIRM_TICKS          2

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
// chỉ ~70-80 seg/tick nên 1 gói mất đọc thành ~1.3% (>0.5% trigger) — sai. Với
// 5×70=350 seg thì 1 gói = 0.29%. VIDEO only. [PORT DEVIATION]
#define ABR_LOSS_WINDOW_TICKS                  5
// Số tick LIÊN TIẾP throughput==0 mới coi là sụp thật. [PORT DEVIATION]
#define ABR_ZERO_THROUGHPUT_CONFIRM_TICKS      2
// Recovery-hold: khi đang KẸT ở MIN (current <= MIN), phải có bấy nhiêu tick
// SẠCH LIÊN TIẾP (mọi gate ramp-up đều ok: loss/qdelay/rtt/jitter/cooldown) mới
// cho leo lên lại. Chống "đơ đơ" — 1 tick sạch đơn lẻ (buffer chưa đẩy hết) vẫn
// leo vào link còn nghẽn. Bất kỳ tick break nào reset về 0. [PORT DEVIATION]
#define ABR_RECOVERY_CONFIRM_TICKS             2
// Số điều kiện xấu liên tiếp để hạ status về POOR_NETWORK. [xbfirm]
#define ABR_POOR_CONDITIONS_TICKS              2
// Đổi bitrate phải khác ít nhất (kbps) này, nếu không thì bỏ qua (no-op). [xbfirm]
#define ABR_MIN_BITRATE_CHANGE_THRESHOLD       30
// Phase 13 (switch-seed): khi 1 instance VỪA thành primary mà CHÍNH link đó khoẻ
// (no-loss tick này + score mạnh + qdelay thấp + usable) và bitrate đang kẹt ở
// sàn, seed thẳng lên mức này thay vì bò 30->100->... (giây under-shoot sau switch
// eth-yếu -> eth-mạnh). Bị min() với cap đã học. 500 = cùng mức telemetry kick
// (ABR_TELEMETRY_KICK_BITRATE_KBPS). [PORT DEVIATION]
#define ABR_SWITCH_SEED_HEALTHY_KBPS           500
// Phase 13 (switch-seed): tại sàn nhưng link KHÔNG khoẻ — nhấc nhẹ lên mức này
// (một nấc, KHÔNG nhảy tới bootstrap) để yếu link không bị dội bitrate. Bị min()
// với cap đã học. [PORT DEVIATION]
#define ABR_SWITCH_SEED_FLOOR_KBPS             100
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

// Mốc bỏ trần bằng GPS (604): khi vị trí HIỆN TẠI cách vị trí lúc HỌC trần
// ≥ ngưỡng này (mét) → môi trường đã đổi → được phép bỏ trần. 500 m ≫ jitter GPS
// (~mét) nên ngưỡng TỰ là debounce, không cần đếm tick. Dùng staleness chung
// ABR_STRENGTH_STALE_MS (5s) cho fix cũ. [PORT DEVIATION]
#define ABR_GPS_MOVE_METERS                    500.0

#endif // ABRTUNING_H
