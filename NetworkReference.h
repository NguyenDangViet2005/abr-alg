#ifndef NETWORKREFERENCE_H
#define NETWORKREFERENCE_H

#include <QElapsedTimer>
#include "ABRTuning.h"   // ABR_STRENGTH_STALE_MS / _CONFIRM_TICKS

// Per-interface environment reference for the ABR's ceiling gate: radio strength
// (packet 603) AND position (packet 604).
//
// Holds ONE interface's latest radio strength (603), the strength in force when a
// ceiling was LEARNED, the vehicle's latest GPS fix (604, GLOBAL — the same fix
// every instance sees) and the position at the last ceiling-LEARN. Answers the
// read-only verdict environmentChanged() — "has the environment genuinely
// changed since we learned the ceiling?" — as an OR of two sub-rules: the radio
// recovered (strength) OR the vehicle moved ≥ ABR_GPS_MOVE_METERS (GPS).
// XBAdaptiveBitrateStreaming::allowExceedCeiling() consults it; true lifts the
// learned ceiling and re-probes.
//
// No QObject, no I/O of its own: values are PUSHED in from the ABR slots that the
// factory routes/broadcasts to, so the class is pure logic and testable in isolation.
//
// Strength scale (0..4, weak -> strong; 9 = no info). The router reserves packets
// 603 and 604 for these but emits nothing yet (no RSSI/GPS source), so today the
// ref stays cold and environmentChanged() is false — the fail-closed default.
class NetworkReference
{
public:
    NetworkReference();

    // Each 603 tick: validate, refresh staleness, advance the debounce.
    void setStrength(int strength);

    // Each 604 tick: store the fix, refresh its staleness timer, and — if the
    // ceiling was learned with NO fix — adopt this fix as the anchor. GLOBAL:
    // the same call is made on every instance (GPS has no interface name).
    void setGps(double lat, double lon);

    // Called at the LEARN event (applyOvershootCeiling) — snapshot the strength
    // AND position NOW. Unconditional: a re-learn at the same plateau must still
    // refresh. A no-fix learn invalidates the GPS anchor (no "we moved" datum).
    void markBaseline();

    // Read-only verdict: may the ceiling be lifted? No side effects.
    bool environmentChanged() const;

    // True while the primary verdict CAN work: a FRESH reading of an ACTUAL
    // level. A 603 stream carrying NO_INFO(9), or any stale/dead source, is NOT
    // usable — environmentChanged() is permanently false in exactly those cases,
    // so the ceiling-escape FALLBACK is gated on !hasUsableStrength() and the
    // ABR re-probes on a backoff instead of holding forever. (Gating on "a 603
    // was ever seen" would re-create the held-forever defect: the router sending
    // 603<name>#9 — modem offline — would disable the fallback permanently.)
    bool hasUsableStrength() const { return !isStale() && m_strength != NO_INFO; }

    // Sibling of hasUsableStrength() for GPS: true while a FRESH fix exists. The
    // ceiling-escape FALLBACK stands down when EITHER a usable strength OR a
    // usable GPS exists (a real signal owns the escape then). GPS absent today →
    // false → fallback behaviour unchanged.
    bool hasUsableGps() const { return m_lastGpsMs != 0 && !gpsStale(); }

    // Fresh session: forget everything.
    void reset();

    // Wire contract constants (shared with the router's StatCellOrder contract).
    static constexpr int NO_INFO      = 9;  // modem offline / no measurement
    static constexpr int STRENGTH_MIN = 0;  // weakest
    static constexpr int STRENGTH_MAX = 4;  // strongest

private:
    // Tuning: ABR_STRENGTH_STALE_MS / ABR_STRENGTH_CONFIRM_TICKS (see ABRTuning.h).
    // ABR_STRENGTH_STALE_MS: no 603 within this window ⇒ no info (fail-closed).
    // ABR_STRENGTH_CONFIRM_TICKS: the raw condition must hold for this many CONSECUTIVE fresh
    // ticks. Tick-count, NOT wall-clock: a wall-clock "held since" timer keeps
    // AGING while ticks stop arriving, so a short blip followed by sender silence
    // would satisfy "held 5s" once wall time passes with no tick to falsify it.
    // Counting fresh ticks freezes under silence, and environmentChanged() also
    // fails when stale — so a blip+silence can never confirm.

    bool isStale() const;
    bool gpsStale() const;

    // The two sub-rules of environmentChanged(). Split so each is legible and
    // the OR at the call site reads plainly.
    bool strengthRecovered() const;
    bool gpsMoved() const;

    int    m_strength       = NO_INFO;  // only ever {0..4} or NO_INFO
    int    m_baseline       = NO_INFO;  // strength at the last ceiling-learn
    bool   m_hasBaseline    = false;    // false until a ceiling is learned
    qint64 m_lastStrengthMs = 0;        // 0 = never received a 603
    int    m_condTicks      = 0;        // consecutive FRESH ticks with raw == true

    // ── GPS (604) ────────────────────────────────────────────────────────
    // m_anchorLat/Lon + m_gpsAnchorValid: the position at the last ceiling-LEARN.
    //   Invalid when learned with no live fix — then the FIRST real fix seeds it
    //   (setGps), NOT an auto-lift: the first fix carries no "we moved" evidence.
    // m_fixLat/Lon: the latest fix. m_lastGpsMs: 0 = never received a 604.
    double m_anchorLat      = 0.0;
    double m_anchorLon      = 0.0;
    bool   m_gpsAnchorValid = false;
    double m_fixLat         = 0.0;
    double m_fixLon         = 0.0;
    qint64 m_lastGpsMs      = 0;

    QElapsedTimer m_clock;              // monotonic; started in the ctor
};

#endif // NETWORKREFERENCE_H
