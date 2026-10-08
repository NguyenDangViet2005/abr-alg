#ifndef NETWORKREFERENCE_H
#define NETWORKREFERENCE_H

#include <QElapsedTimer>

// Per-interface radio-strength reference for the ABR's ceiling gate.
//
// Holds ONE interface's latest radio strength (packet 603), the strength that was
// in force when a ceiling was LEARNED, and answers the read-only verdict
// environmentChanged() — "has the radio genuinely recovered since we learned the
// ceiling?". XBAdaptiveBitrateStreaming::allowExceedCeiling() consults it; true
// lifts the learned ceiling and re-probes.
//
// No QObject, no I/O of its own: values are PUSHED in from the ABR slot that the
// factory routes to, so the class is pure logic and testable in isolation.
//
// Strength scale (0..4, weak -> strong; 9 = no info). The router reserves packet
// 603 for this but emits nothing yet (no RSSI source), so today the ref stays
// cold and environmentChanged() is false — the fail-closed default.
class NetworkReference
{
public:
    NetworkReference();

    // Each 603 tick: validate, refresh staleness, advance the debounce.
    void setStrength(int strength);

    // Called at the LEARN event (applyOvershootCeiling) — snapshot the strength
    // NOW. Unconditional: a re-learn at the same plateau must still refresh.
    void markBaseline();

    // Read-only verdict: may the ceiling be lifted? No side effects.
    bool environmentChanged() const;

    // Fresh session: forget everything.
    void reset();

    // Wire contract constants (shared with the router's StatCellOrder contract).
    static constexpr int NO_INFO      = 9;  // modem offline / no measurement
    static constexpr int STRENGTH_MIN = 0;  // weakest
    static constexpr int STRENGTH_MAX = 4;  // strongest

private:
    // No 603 within this window ⇒ no info (fail-closed).
    static constexpr qint64 STALE_MS = 5000;

    // The raw condition must hold for this many CONSECUTIVE fresh ticks.
    // Tick-count, NOT wall-clock: a wall-clock "held since" timer keeps AGING
    // while ticks stop arriving, so a short blip followed by sender silence would
    // satisfy "held 5s" once wall time passes with no tick to falsify it.
    // Counting fresh ticks freezes under silence, and environmentChanged() also
    // fails when stale — so a blip+silence can never confirm.
    static constexpr int CONFIRM_TICKS = 5;

    bool isStale() const;

    int    m_strength       = NO_INFO;  // only ever {0..4} or NO_INFO
    int    m_baseline       = NO_INFO;  // strength at the last ceiling-learn
    bool   m_hasBaseline    = false;    // false until a ceiling is learned
    qint64 m_lastStrengthMs = 0;        // 0 = never received a 603
    int    m_condTicks      = 0;        // consecutive FRESH ticks with raw == true
    QElapsedTimer m_clock;              // monotonic; started in the ctor
};

#endif // NETWORKREFERENCE_H
