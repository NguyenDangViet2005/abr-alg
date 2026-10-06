// Unit test for the pure decision core, decideBitrate().
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure, so it can be compiled and run alone. The function
// under test is pure — no socket, no instance, no timer — which is the whole
// reason it was split out.
//
// Build (adjust include paths for your Qt):
//   g++ -std=c++17 -I.. -I$QT_INC -I$QT_INC/QtCore \
//       tst_decidebitrate.cpp ../ABRDecision.cpp ../RouterInterfaceStat.h \
//       -lQt5Core -o tst_decidebitrate
//
// The project rule is that the user builds; this file only has to be correct.

#include "../ABRDecision.h"
#include <QDebug>

static int g_failures = 0;

// decideBitrate returns AbrDecision (kbps + branch + reason). These tests assert
// on the kbps field — the branch/reason are exercised implicitly and checked by
// the QS encoder test on the wire side. Taking the struct here keeps every call
// site below unchanged.
static void expectEq(AbrDecision got, int want, const char *what)
{
    if (got.kbps != want) {
        ++g_failures;
        qWarning().nospace() << "FAIL " << what << ": got " << got.kbps << ", want " << want;
    } else {
        qInfo().nospace() << "ok   " << what << " = " << got.kbps;
    }
}

static void expectReason(AbrDecision got, AbrReason want, const char *what)
{
    if (got.reason != want) {
        ++g_failures;
        qWarning().nospace() << "FAIL " << what << ": reason " << static_cast<int>(got.reason)
                             << ", want " << static_cast<int>(want);
    } else {
        qInfo().nospace() << "ok   " << what << " reason=" << static_cast<int>(got.reason);
    }
}

// A clean, healthy video row; each test perturbs one field. score=90 sits above
// ABR_SCORE_UP_MIN (70), so the wrapper would count it as a "high" sample.
static RouterInterfaceStat healthyVideo()
{
    RouterInterfaceStat v;
    v.connType = ConnType::VIDEO;
    v.interfaceName = "wwan1";
    v.isConnected = true;
    v.isMptcp = true;
    v.usable = true;
    v.score = 90.0;
    v.trend = TREND_STABLE;
    v.throughputKbps = 3000.0;
    v.lossRatePct = 0.0;
    v.rttMs = 30.0;
    v.minRttMs = 20.0;
    v.queuingDelayMs = 10.0;
    v.jitterMs = 3.0;
    return v;
}

static RouterInterfaceStat cleanC2()
{
    RouterInterfaceStat c;
    c.connType = ConnType::C2;
    c.interfaceName = "wwan1";
    c.lossRatePct = 0.1;
    c.queuingDelayMs = 5.0;
    return c;
}

// Streak counters for a call. Defaults are all-zero (a fresh tick).
static AbrCounters counters(int notUsable = 0, int c2Loss = 0, int low = 0, int high = 0)
{
    AbrCounters k;
    k.notUsableTicks  = notUsable;
    k.c2LossTicks     = c2Loss;
    k.scoreLowStreak  = low;
    k.scoreHighStreak = high;
    return k;
}

// A streak long enough to fire (the wrapper would have counted it).
static const int FULL = ABR_SCORE_STREAK_TICKS;

static const int MAX_ABR = 6000;
static const int CUR = 1000;

int main()
{
    const RouterInterfaceStat c = cleanC2();

    // ── link down ────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.isConnected = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0), false, 9999, MAX_ABR), CUR,
                 "down link, tick 0 -> hold");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(1), false, 9999, MAX_ABR), CUR,
                 "down link, tick 1 -> hold");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(2), false, 9999, MAX_ABR), QOS_PAUSE,
                 "down link, tick 2 -> PAUSE");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.isMptcp = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(2), false, 9999, MAX_ABR), QOS_PAUSE,
                 "no mptcp, tick 2 -> PAUSE");
    }

    // ── !usable -> NOT_AVAILABLE (not 0) ─────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.usable = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 9999, MAX_ABR), QOS_NOT_AVAILABLE,
                 "!usable -> NOT_AVAILABLE (-1)");
    }

    // ── KTO ──────────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 1.0;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 9999, MAX_ABR), CUR / 2,
                 "score<=1 -> halve");
    }

    // ── FAST decrease branches (1 tick) ──────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 6.0;      // > 5 -> severe
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 9999, MAX_ABR), CUR / 2,
                 "loss>5 -> halve");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 1.0;      // > 0.8, < 5
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 9999, MAX_ABR), CUR / 2,
                 "loss in (0.8,5) -> halve");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.trend = TREND_RAPID_DECREASE;   // clean loss
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 9999, MAX_ABR),
                 static_cast<int>(CUR * ABR_TREND_RAPID_DEC_GAIN), "rapid-decrease -> x0.85");
    }
    // There is NO queue/congestion decrease branch (2026-10-06). The router's
    // queuingDelayMs is Little's Law (backlog / throughput) and its divisor is
    // the ABR's own output, so it fed back on itself: the 2026-10-05 log drove
    // 225 -> 36 and the 2026-10-06 log hit the 10 kbps floor while rtt-minRtt
    // stayed flat at 1-15ms. The queue now reaches the decision only through
    // score (computeRawScore's wRTT*(rtt-minRtt) term) -> the score streak.

    // ── SLOW decrease branch: score streak low (3 samples) ───────────────
    {
        RouterInterfaceStat v = healthyVideo();   // score value itself is not read for the streak
        // Below the streak length -> HOLD, not a decrease.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 2), false, 9999, MAX_ABR), CUR,
                 "low streak 2 -> hold (not yet)");
        // Full streak -> x0.80.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, FULL), false, 9999, MAX_ABR),
                 static_cast<int>(CUR * ABR_SCORE_DEC_GAIN), "low streak 3 -> x0.80");
        // Floor still applies: 12 * 0.8 = 9 -> MIN 10.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 12, counters(0, 0, FULL), false, 9999, MAX_ABR),
                 ABR_MIN_BITRATE_KBPS, "low streak 3 below MIN -> floors at 10");
    }
    // FAST wins over SLOW on the same tick: a loss tick with a saturated low
    // streak still halves (LOSS_HIGH), never the slow x0.80.
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 1.0;      // > 0.8 -> halve
        AbrDecision d = decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, FULL), false, 9999, MAX_ABR);
        expectEq(d, CUR / 2, "loss + saturated low streak -> HALVE (fast wins)");
        expectReason(d, AbrReason::LOSS_HIGH, "loss + saturated low streak");
    }

    // ── PAUSE is never clamped up to MIN ─────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.isConnected = false;
        // CUR above MIN; the same call at a tiny bitrate must still be 0.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 100, counters(2), false, 9999, MAX_ABR), QOS_PAUSE,
                 "PAUSE not clamped to MIN");
    }

    // ── SLOW increase branch: score streak high (3 samples) ──────────────
    {
        RouterInterfaceStat v = healthyVideo();
        // Full high streak + FRESH + cooldown -> +100.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS, "high streak 3 + FRESH + cooldown -> +100");
        expectEq(decideBitrate(v, c, C2Mode::DELAY_ONLY, CUR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS, "high streak 3 + DELAY_ONLY -> +100");
        // Below the streak length -> HOLD, reason SCORE_HIGH (ramp withheld by
        // the streak, not by score).
        AbrDecision d = decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 0, 2), false, 3000, MAX_ABR);
        expectEq(d, CUR, "high streak 2 -> hold (not yet)");
        expectReason(d, AbrReason::SCORE_HIGH, "high streak 2 -> SCORE_HIGH");
        // C2 must give a reference: STALE/NEVER_SEEN block the ramp.
        expectEq(decideBitrate(v, c, C2Mode::STALE, CUR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR), CUR,
                 "STALE blocks INCREASE");
        expectEq(decideBitrate(v, c, C2Mode::NEVER_SEEN, CUR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR), CUR,
                 "NEVER_SEEN blocks INCREASE");
        // Cooldown gate: everything else permits the ramp, but only 1999ms have
        // passed — the 2000ms cooldown alone must hold.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 0, FULL), false, 1999, MAX_ABR), CUR,
                 "high streak 3 + FRESH but within cooldown -> hold");
    }

    // ── cameraCollapsed blocks INCREASE, never DECREASE ──────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 0, FULL), true, 3000, MAX_ABR), CUR,
                 "cameraCollapsed -> hold (ramp blocked)");
        // And it must NOT block a decrease: a lossy tick still halves.
        v.lossRatePct = 1.0;   // > 0.8 -> halve
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), true, 9999, MAX_ABR), CUR / 2,
                 "cameraCollapsed + loss -> still halve (decrease unaffected)");
    }

    // ── raw rtt / jitter / qdelay are NOT read by the decision ───────────
    // The core reads score-streaks, loss and trend only. rtt, minRtt, jitter and
    // queuingDelayMs are NOT consulted — score already aggregates the first three
    // (computeRawScore charges wRTT*(rtt-minRtt), jitter and loss), and qdelay is
    // the feedback trap that was removed. Perturbing those raw fields must not
    // change the outcome: the high streak alone permits the ramp.
    {
        RouterInterfaceStat v = healthyVideo();
        v.queuingDelayMs = 1187.0;   // huge ghost, never consulted
        v.rttMs = 90.0;              // inflation 70, never consulted
        v.jitterMs = 20.0;           // raised, never consulted
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS,
                 "rtt/jitter/qdelay fields not consulted -> ramp by streak alone");
    }

    // ── ceiling / floor clamping ─────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, c, C2Mode::FRESH, MAX_ABR, counters(0, 0, 0, FULL), false, 3000, MAX_ABR), MAX_ABR,
                 "at maxAbr -> hold (no overshoot)");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, MAX_ABR - 50, counters(0, 0, 0, FULL), false, 3000, MAX_ABR), MAX_ABR,
                 "+100 step clamps to maxAbr");
    }

    // ── deadband: the streak, not the score, is what the core reads ──────
    // A score in [45,70) is neither "low" nor "high" — the WRAPPER clears both
    // streaks there. The core never sees a non-zero streak from a deadband
    // sample, so all-zero counters must HOLD regardless of the score value.
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 60.0;   // deadband; wrapper would leave both streaks 0
        AbrDecision d = decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 3000, MAX_ABR);
        expectEq(d, CUR, "deadband score, no streak -> hold");
        expectReason(d, AbrReason::SCORE_LOW, "deadband score -> SCORE_LOW hold reason");
    }

    // ── unknown C2 blocks INCREASE but NEVER blocks DECREASE ─────────────
    // The other half of the C2 contract: STALE/NEVER_SEEN withhold the
    // reference (so no ramp) but must still let a loss backoff through.
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 1.0;      // > 0.8 -> halve
        expectEq(decideBitrate(v, c, C2Mode::STALE, CUR, counters(), false, 9999, MAX_ABR), CUR / 2,
                 "STALE still DECREASES on loss");
        expectEq(decideBitrate(v, c, C2Mode::NEVER_SEEN, CUR, counters(), false, 9999, MAX_ABR), CUR / 2,
                 "NEVER_SEEN still DECREASES on loss");
    }

    // ── C2-loss gate: a lossy PATH pauses the video ──────────────────────
    //
    // The C2 row is the only non-TCP-hidden loss signal, so >10% there means
    // the path itself is dropping. Debounced to ABR_C2_LOSS_PAUSE_TICKS (3)
    // consecutive ticks, FRESH only. C2 acts on the PAUSE only — the ramp is
    // gated by the score streak, so a high streak still ramps during the two
    // ticks before the pause lands.
    {
        RouterInterfaceStat cLossy = cleanC2();
        cLossy.lossRatePct = 12.0;   // > ABR_C2_LOSS_PAUSE_PCT (10)
        RouterInterfaceStat v = healthyVideo();
        // Ticks 0 and 1: below the debounce -> not paused; high streak ramps.
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, counters(0, 0, 0, FULL), false, 9999, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS, "C2 loss tick 0 -> not paused (streak ramps)");
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, counters(0, 1, 0, FULL), false, 9999, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS, "C2 loss tick 1 -> not paused (streak ramps)");
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, counters(0, 2, 0, FULL), false, 9999, MAX_ABR),
                 QOS_PAUSE, "C2 loss tick 2 -> PAUSE (3 in a row)");
        // At the threshold exactly is NOT above it -> no pause.
        RouterInterfaceStat cAt = cleanC2();
        cAt.lossRatePct = ABR_C2_LOSS_PAUSE_PCT;
        expectEq(decideBitrate(v, cAt, C2Mode::FRESH, CUR, counters(0, 5, 0, FULL), false, 9999, MAX_ABR),
                 CUR + ABR_UP_STEP_KBPS, "C2 loss == threshold -> not paused (strictly greater)");
    }
    // A STALE/NEVER_SEEN C2 is "unknown", not "lossy": it must NOT pause, even
    // with a lossy row on file and a saturated counter — and, withholding the
    // reference, it blocks the ramp.
    {
        RouterInterfaceStat cLossy = cleanC2();
        cLossy.lossRatePct = 50.0;
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, cLossy, C2Mode::STALE, CUR, counters(0, 9, 0, FULL), false, 9999, MAX_ABR), CUR,
                 "STALE C2 lossy -> hold (unknown is not lossy; ramp blocked)");
        expectEq(decideBitrate(v, cLossy, C2Mode::NEVER_SEEN, CUR, counters(0, 9, 0, FULL), false, 9999, MAX_ABR), CUR,
                 "NEVER_SEEN C2 lossy -> hold (unknown is not lossy; ramp blocked)");
    }

    // ── CEILING memory caps the ramp (2026-10-06) ────────────────────────
    // rampCapKbps is the wrapper's recorded failed bitrate × ABR_CEILING_BACKOFF.
    // It gates INCREASE only; the last positional arg defaults to 0 (no cap) so
    // every call above is unchanged. Field case: died at 1248 -> cap 1061.
    {
        RouterInterfaceStat v = healthyVideo();
        const int CAP = 1061;   // 1248 × 0.85
        // Next step would exceed the cap -> HOLD, reason CEILING.
        AbrDecision d = decideBitrate(v, c, C2Mode::FRESH, 1048, counters(0, 0, 0, FULL),
                                      false, 3000, MAX_ABR, CAP);
        expectEq(d, 1048, "cap 1061, at 1048 (+100 > cap) -> hold");
        expectReason(d, AbrReason::CEILING, "cap blocks the ramp");
        // The step fits exactly -> UP (parks AT the cap, equal allowed).
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 961, counters(0, 0, 0, FULL), false, 3000, MAX_ABR, CAP),
                 1061, "cap 1061, at 961 (+100 == cap) -> UP, parks at cap");
        // Well below -> UP.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 900, counters(0, 0, 0, FULL), false, 3000, MAX_ABR, CAP),
                 1000, "cap 1061, at 900 -> UP (fits)");
        // No memory -> no cap.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 1048, counters(0, 0, 0, FULL), false, 3000, MAX_ABR, 0),
                 1148, "rampCapKbps 0 -> no cap, UP");
        // The cap NEVER blocks a decrease: a lossy tick above the cap still halves.
        RouterInterfaceStat vl = healthyVideo();
        vl.lossRatePct = 1.0;
        AbrDecision dl = decideBitrate(vl, c, C2Mode::FRESH, 1248, counters(), false, 9999, MAX_ABR, CAP);
        expectEq(dl, 624, "cap does not block DECREASE (loss still halves)");
        expectReason(dl, AbrReason::LOSS_HIGH, "cap does not block DECREASE");
    }

    // ── ABR_SCORE_DEC_MIN 45: the slow-low branch fires at score 50 ───────
    // Score 50 is below 55 (the old floor) but above 45, so the WRAPPER's
    // streak logic (score < DEC_MIN) is what decides. The core reads only the
    // streak, so this is a constants check: a 3-low streak at a score that the
    // OLD threshold would have held now fires ×0.80.
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 50.0;   // < 55, >= 45 -> "low" only under the new floor
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, 2), false, 9999, MAX_ABR), CUR,
                 "score 50, low streak 2 -> hold");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(0, 0, FULL), false, 9999, MAX_ABR),
                 static_cast<int>(CUR * ABR_SCORE_DEC_GAIN), "score 50, low streak 3 -> x0.80 (new floor)");
    }

    // ── hold ─────────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 50.0;   // below the ramp floor, above KTO, no loss, no streak
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, counters(), false, 3000, MAX_ABR), CUR,
                 "low score, no loss, no streak -> hold");
    }

    if (g_failures == 0) {
        qInfo() << "ALL PASS";
        return 0;
    }
    qWarning() << g_failures << "FAILURE(S)";
    return 1;
}
