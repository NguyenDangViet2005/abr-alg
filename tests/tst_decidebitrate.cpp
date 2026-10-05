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
#include <cmath>

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

// A clean, healthy video row; each test perturbs one field.
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

static const int MAX_ABR = 6000;
static const int CUR = 1000;

int main()
{
    const RouterInterfaceStat c = cleanC2();

    // ── link down ────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.isConnected = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), CUR,
                 "down link, tick 0 -> hold");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 1, 0, 9999, MAX_ABR), CUR,
                 "down link, tick 1 -> hold");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 2, 0, 9999, MAX_ABR), QOS_PAUSE,
                 "down link, tick 2 -> PAUSE");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.isMptcp = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 2, 0, 9999, MAX_ABR), QOS_PAUSE,
                 "no mptcp, tick 2 -> PAUSE");
    }

    // ── !usable -> NOT_AVAILABLE (not 0) ─────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.usable = false;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), QOS_NOT_AVAILABLE,
                 "!usable -> NOT_AVAILABLE (-1)");
    }

    // ── KTO ──────────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 1.0;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), CUR / 2,
                 "score<=1 -> halve");
    }

    // ── decrease branches ────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 6.0;      // > 5 -> severe
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), CUR / 2,
                 "loss>5 -> halve");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 1.0;      // > 0.8, < 5
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), CUR / 2,
                 "loss in (0.8,5) -> halve");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.queuingDelayMs = 500.0; // > 400
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR),
                 static_cast<int>(CUR * ABR_QDELAY_GAIN), "qdelay>400 -> x0.8");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.trend = TREND_RAPID_DECREASE;   // clean loss/qdelay
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR),
                 static_cast<int>(CUR * ABR_TREND_RAPID_DEC_GAIN), "rapid-decrease -> x0.85");
    }

    // ── PAUSE is never clamped up to MIN ─────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.isConnected = false;
        // CUR above MIN; the same call at a tiny bitrate must still be 0.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 100, 2, 0, 9999, MAX_ABR), QOS_PAUSE,
                 "PAUSE not clamped to MIN");
    }

    // ── increase gate ────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR + ABR_UP_STEP_KBPS,
                 "clean + FRESH + cooldown -> +100");
        expectEq(decideBitrate(v, c, C2Mode::DELAY_ONLY, CUR, 0, 0, 3000, MAX_ABR), CUR + ABR_UP_STEP_KBPS,
                 "clean + DELAY_ONLY -> +100");
        expectEq(decideBitrate(v, c, C2Mode::STALE, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "STALE blocks INCREASE");
        expectEq(decideBitrate(v, c, C2Mode::NEVER_SEEN, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "NEVER_SEEN blocks INCREASE");
        // Cooldown gate: everything else permits the ramp, but only 1999ms
        // have passed — the 2000ms cooldown alone must hold.
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 1999, MAX_ABR), CUR,
                 "clean + FRESH but within cooldown -> hold");
    }

    // ── linkClean sub-conditions gate the ramp (each must hold alone) ────
    {
        RouterInterfaceStat v = healthyVideo();
        v.queuingDelayMs = 150.0;   // >= ABR_QDELAY_LOW_MS (100), < 400
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "qdelay 150 (>= LOW) blocks INCREASE");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.rttMs = 90.0;             // rtt - minRtt = 70 >= 50
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "rtt inflation >= 50 blocks INCREASE");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        v.jitterMs = 20.0;          // >= 15
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "jitter >= 15 blocks INCREASE");
    }

    // ── ceiling / floor clamping ─────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, c, C2Mode::FRESH, MAX_ABR, 0, 0, 3000, MAX_ABR), MAX_ABR,
                 "at maxAbr -> hold (no overshoot)");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, MAX_ABR - 50, 0, 0, 3000, MAX_ABR), MAX_ABR,
                 "+100 step clamps to maxAbr");
    }
    {
        RouterInterfaceStat v = healthyVideo();
        // MIN is 10 now, so a plain halve of a normal bitrate stays ABOVE the
        // floor (300/2 = 150). To exercise the floor the halve must land below
        // it: 15/2 = 7 < 10.
        v.lossRatePct = 1.0;        // > 0.8 -> halve
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 15, 0, 0, 3000, MAX_ABR), ABR_MIN_BITRATE_KBPS,
                 "halve below MIN -> floors at 10");
        expectEq(decideBitrate(v, c, C2Mode::FRESH, 300, 0, 0, 3000, MAX_ABR), 150,
                 "halve above MIN -> 150, not floored");
    }

    // ── the trend-order bug: DECREASE must NOT ramp ──────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.trend = TREND_DECREASE;
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "TREND_DECREASE blocks INCREASE (trend<=STABLE gate)");
    }

    // ── unknown C2 blocks INCREASE but NEVER blocks DECREASE ─────────────
    // The other half of the C2 contract: STALE/NEVER_SEEN withhold the
    // reference (so no ramp) but must still let a loss/qdelay backoff through.
    {
        RouterInterfaceStat v = healthyVideo();
        v.lossRatePct = 1.0;      // > 0.8 -> halve
        expectEq(decideBitrate(v, c, C2Mode::STALE, CUR, 0, 0, 9999, MAX_ABR), CUR / 2,
                 "STALE still DECREASES on loss");
        expectEq(decideBitrate(v, c, C2Mode::NEVER_SEEN, CUR, 0, 0, 9999, MAX_ABR), CUR / 2,
                 "NEVER_SEEN still DECREASES on loss");
    }

    // ── C2-loss gate: a lossy PATH pauses the video ──────────────────────
    //
    // The C2 row is the only non-TCP-hidden loss signal, so >10% there means
    // the path itself is dropping. Debounced to ABR_C2_LOSS_PAUSE_TICKS (3)
    // consecutive ticks, FRESH only.
    {
        RouterInterfaceStat cLossy = cleanC2();
        cLossy.lossRatePct = 12.0;   // > ABR_C2_LOSS_PAUSE_PCT (10)
        RouterInterfaceStat v = healthyVideo();
        // Ticks 0 and 1: still below the debounce -> HOLD (positive, video runs).
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, 0, 0, 9999, MAX_ABR), CUR,
                 "C2 loss tick 0 -> hold (debounce not met)");
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, 0, 1, 9999, MAX_ABR), CUR,
                 "C2 loss tick 1 -> hold (debounce not met)");
        expectEq(decideBitrate(v, cLossy, C2Mode::FRESH, CUR, 0, 2, 9999, MAX_ABR), QOS_PAUSE,
                 "C2 loss tick 2 -> PAUSE (3 in a row)");
        // At the threshold exactly is NOT above it -> no pause.
        RouterInterfaceStat cAt = cleanC2();
        cAt.lossRatePct = ABR_C2_LOSS_PAUSE_PCT;
        expectEq(decideBitrate(v, cAt, C2Mode::FRESH, CUR, 0, 5, 9999, MAX_ABR), CUR,
                 "C2 loss == threshold -> hold (strictly greater)");
    }
    // A STALE/NEVER_SEEN C2 is "unknown", not "lossy": it must NOT pause, even
    // with a lossy row on file and a saturated counter.
    {
        RouterInterfaceStat cLossy = cleanC2();
        cLossy.lossRatePct = 50.0;
        RouterInterfaceStat v = healthyVideo();
        expectEq(decideBitrate(v, cLossy, C2Mode::STALE, CUR, 0, 9, 9999, MAX_ABR), CUR,
                 "STALE C2 lossy -> hold (unknown is not lossy)");
        expectEq(decideBitrate(v, cLossy, C2Mode::NEVER_SEEN, CUR, 0, 9, 9999, MAX_ABR), CUR,
                 "NEVER_SEEN C2 lossy -> hold (unknown is not lossy)");
        expectEq(decideBitrate(v, cLossy, C2Mode::DELAY_ONLY, CUR, 0, 9, 9999, MAX_ABR), CUR,
                 "DELAY_ONLY (C2 absent) -> hold (no reference to judge)");
    }

    // ── hold ─────────────────────────────────────────────────────────────
    {
        RouterInterfaceStat v = healthyVideo();
        v.score = 50.0;   // below the ramp gate, above KTO, no loss/qdelay
        expectEq(decideBitrate(v, c, C2Mode::FRESH, CUR, 0, 0, 3000, MAX_ABR), CUR,
                 "low score, no loss -> hold");
    }

    if (g_failures == 0) {
        qInfo() << "ALL PASS";
        return 0;
    }
    qWarning() << g_failures << "FAILURE(S)";
    return 1;
}
