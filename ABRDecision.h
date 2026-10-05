#ifndef ABRDECISION_H
#define ABRDECISION_H

#include <QtGlobal>
#include "RouterInterfaceStat.h"

// Pure bitrate-decision core, ported from xbfirm's XBAdaptiveBitrateStreaming
// but reading the router's ALREADY-COMPUTED per-interface rows instead of raw
// tcpi counters. Kept as a free function so it can be unit-tested without a
// socket, a timer, or an instance — every piece of state it needs is a
// parameter. See plans/reports/brainstorm-261002-1141.
//
// The router sends FACTS (quality, load, flags); this decides a bitrate. Note
// `throughputKbps` is LOAD (bytes actually sent), NOT capacity — it must never
// be used as a ceiling, or the algorithm deadlocks at its own send rate.

// Which C2 reference state applies to this tick. Critical: "C2 did not arrive
// this tick" is NOT "C2 is clean" — conflating them labels network loss as
// self-inflicted. Four states, not two.
enum class C2Mode : int {
    FRESH = 0,       // C2 row arrived this tick -> usable reference
    STALE = 1,       // C2 seen before, absent this tick -> unknown, NOT clean
    NEVER_SEEN = 2,  // declared present globally, never seen -> block increase
    DELAY_ONLY = 3   // C2 not configured at all -> delay-based, allow increase
};

// Return contract, matching the wire:
//   -1 QOS_NOT_AVAILABLE : cannot decide -> caller must NOT emit
//    0 QOS_PAUSE         : pause video (== VIDEO_DISABLED_BITRATE_KBPS)
//   >0                    : proposed kbps
// -1 is distinct from 0: "no idea" is not "stop the video".
constexpr int QOS_NOT_AVAILABLE = -1;
constexpr int QOS_PAUSE         = 0;

// ── Branch / reason (report §3.1) ───────────────────────────────────────────
//
// WHY the bitrate moved this tick. Values are the QS WIRE values (see
// dev/QoSDebugProtocol.h), so a decision can be published verbatim with no
// translation table — and the wire header aliases these names back, keeping a
// single definition site. Adding a value means editing only this enum.
//
// HOLD is the steady state; NOT_AVAILABLE means "cannot decide" and never
// reaches a consumer as a bitrate. TO_STUCK_OBSERVE / RESET_REQUEST belong to
// the Part B stuck path, emitted as events rather than as a tick's branch.
enum class AbrBranch : int {
    HOLD             = 0,
    UP               = 1,
    HALVE            = 2,
    MUL              = 3,
    PAUSE            = 4,
    NOT_AVAILABLE    = 5,
    TO_STUCK_OBSERVE = 6,
    RESET_REQUEST    = 7
};

enum class AbrReason : int {
    NONE            = 0,
    KTO             = 1,
    LOSS_HIGH       = 2,
    QDELAY_400      = 3,
    QDELAY_5        = 4,
    TREND_RAPID_DEC = 5,
    LINK_DEAD       = 6,
    NOT_USABLE      = 7,
    STALE           = 8,
    COOLDOWN_OK     = 9,
    C2_BLOCK_UP     = 10,
    SCORE_LOW       = 11,
    STUCK_OBSERVE   = 12,
    RESET_STUCK     = 13,
    C2_LOSS_HIGH    = 14
};

// What decideBitrate returns: the bitrate AND the branch/reason that produced
// it, so a consumer (the QS debug channel) publishes the decision beside the
// inputs it was made from. `kbps` keeps the old int contract — -1, 0, or >0.
struct AbrDecision {
    int       kbps   = QOS_NOT_AVAILABLE;
    AbrBranch branch = AbrBranch::NOT_AVAILABLE;
    AbrReason reason = AbrReason::NONE;
};

// Thresholds (report §7). Named here so the test and the code share one source.
constexpr double ABR_LOSS_HIGH_PCT    = 0.8;
constexpr double ABR_LOSS_SEVERE_PCT  = 5.0;
// C2 reference-path loss, PERCENT. This is the NETWORK's own packet loss: the
// video flow's lossRatePct is TCP-hidden (retransmits mask it), so a lossy path
// only shows up on C2. Above this the path is dropping packets faster than any
// bitrate can cope with, so the video is paused (0) rather than halved — sending
// less into a path losing >10% just wastes the wire. Deliberately stricter than
// ABR_LOSS_SEVERE_PCT (5.0): a video self-report halving at 5% is cheap to
// reverse, but pausing the video is disruptive, so C2 must be clearly bad first.
constexpr double ABR_C2_LOSS_PAUSE_PCT = 10.0;
constexpr double ABR_QDELAY_HIGH_MS   = 400.0;
constexpr double ABR_QDELAY_LOW_MS    = 100.0;
constexpr double ABR_RTT_INFLATION_MS = 50.0;
constexpr double ABR_JITTER_HIGH_MS   = 15.0;
constexpr double ABR_SCORE_UP_MIN     = 85.0;
constexpr int    ABR_UP_STEP_KBPS     = 100;
constexpr qint64 ABR_UP_COOLDOWN_MS   = 2000;
constexpr int    ABR_MIN_BITRATE_KBPS = 10;
// The FLOOR is 10, but two callers must NOT inherit it:
//
//   ABR_INITIAL_BITRATE_KBPS — the value m_bitrate starts at and reset() restores
//     to. 10 would make every interface begin near-silent and ramp from there.
//
//   ABR_OVERDRIVE_MIN_BITRATE_KBPS — the guard that decides when "asking for more
//     than the link delivers" (overdrive) is even meaningful. At 10 that guard
//     is true almost always, arming the stuck-watch on healthy low-rate links.
//
// Both were `ABR_MIN_BITRATE_KBPS` when it was 250 — the floor and "a normal
// starting ask" happened to coincide. Lowering the floor separates them.
constexpr int    ABR_INITIAL_BITRATE_KBPS        = 250;
constexpr int    ABR_OVERDRIVE_MIN_BITRATE_KBPS  = 250;
constexpr int    ABR_PAUSE_AFTER_TICKS = 3;
// Consecutive C2-lossy ticks before the video pauses. A single lossy tick is
// noise; three in a row is a path that is actually dropping. Reuses the same
// debounce length as ABR_PAUSE_AFTER_TICKS (link-down) so both "stop the video"
// gates settle at the same cadence. The counter clears the moment C2 loss falls
// back under the threshold, but resume then goes through the normal INCREASE
// path — i.e. it waits out ABR_UP_COOLDOWN_MS from the pause, exactly as a
// link-dead pause does. It does not jump straight back to the pre-pause rate.
constexpr int    ABR_C2_LOSS_PAUSE_TICKS = 3;
constexpr double ABR_QDELAY_GAIN        = 0.8;
constexpr double ABR_TREND_RAPID_DEC_GAIN = 0.85;

// Freshness / staleness guards (report §6b). Untuned — v1 is log-only.
constexpr qint64 ABR_STALE_WARN_MS  = 5000;   // per-flow silence -> warn, hold
constexpr qint64 ABR_C2_GRACE_MS    = 5000;   // declared-present C2 grace period

// Part B — stuck detection (report §6c).
constexpr double ABR_OVERDRIVE_RATIO    = 1.6;   // throughput < bitrate / ratio
constexpr double ABR_STUCK_QDELAY_HIGH_MS = 400.0;
constexpr int    ABR_STUCK_OBSERVE_TICKS = 5;
constexpr double ABR_STUCK_DRAIN_DROP_PCT = 0.30;
constexpr qint64 ABR_RESET_COOLDOWN_MS  = 30000; // min gap between reset emits

// Trend wire values, matching the router's Trend enum (src/TcpData.h). Local
// copy: XBQoSService is a separate binary and must not include across repos.
//
// ORDER MATTERS. RAPID_INCREASE is 0 and RAPID_DECREASE is 4, so "improving"
// is a LOWER value. The increase gate therefore tests `trend <= TREND_STABLE`
// (rapid-increase/increase/stable), NOT `>=`. The report's `>=` was a bug: it
// admitted DECREASE and RAPID_DECREASE into the ramp-up condition.
constexpr int TREND_RAPID_INCREASE = 0;
constexpr int TREND_INCREASE       = 1;
constexpr int TREND_STABLE         = 2;
constexpr int TREND_DECREASE       = 3;
constexpr int TREND_RAPID_DECREASE = 4;

// decideBitrate — the whole algorithm, pure.
//
//   v              : this interface's VIDEO row (QUALITY + LOAD + flags)
//   c              : this interface's C2 row. Its lossRatePct is the path's own
//                    loss (the video row's is TCP-hidden); read only when
//                    c2Mode == FRESH, else the row is stale/absent and ignored.
//   c2Mode         : resolved reference state (see C2Mode)
//   bitrate        : current bitrate BEFORE this tick
//   notUsableTicks : consecutive ticks the link has been down BEFORE this tick
//   c2LossTicks    : consecutive C2-lossy ticks BEFORE this tick (FRESH only)
//   sinceLastChangeMs : ms since the last bitrate change
//   maxAbr         : ceiling
//
// Returns the proposed bitrate together with the branch/reason that produced
// it. Only the kbps field is the "answer"; branch/reason exist so the QS debug
// channel can publish the decision beside its inputs.
AbrDecision decideBitrate(const RouterInterfaceStat &v, const RouterInterfaceStat &c,
                          C2Mode c2Mode, int bitrate, int notUsableTicks,
                          int c2LossTicks, qint64 sinceLastChangeMs, int maxAbr);

#endif // ABRDECISION_H
