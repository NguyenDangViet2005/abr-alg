#include "NetworkReference.h"

NetworkReference::NetworkReference()
{
    m_clock.start();
}

bool NetworkReference::isStale() const
{
    if (m_lastStrengthMs == 0) return true;              // never got a 603
    return (m_clock.elapsed() - m_lastStrengthMs) > ABR_STRENGTH_STALE_MS;
}

void NetworkReference::setStrength(int strength)
{
    // Trust-boundary validation: this value arrives on the unauthenticated UDP
    // telemetry port, so anything outside {0..4, 9} is coerced to NO_INFO. A
    // rogue/hostile 603<name>#99 therefore cannot force a ceiling lift.
    if (!((strength >= STRENGTH_MIN && strength <= STRENGTH_MAX) || strength == NO_INFO)) {
        strength = NO_INFO;
    }

    m_strength = strength;
    m_lastStrengthMs = m_clock.elapsed();

    // The raw condition: has the radio improved on the baseline?
    bool raw;
    if (!m_hasBaseline) {
        raw = false;                       // nothing learned yet
    } else if (strength == NO_INFO) {
        raw = false;                       // no info now
    } else if (m_baseline == NO_INFO) {
        raw = true;                        // learned BLIND: any real level lifts
    } else {
        raw = (m_baseline < strength);     // strictly higher than at learn time
    }

    // Advance / clear the debounce. Under silence no tick arrives, so the
    // counter simply freezes; staleness is what finally fails the verdict.
    m_condTicks = raw ? (m_condTicks + 1) : 0;
}

void NetworkReference::markBaseline()
{
    // Snapshot the strength in force at the learn instant. A stale snapshot is
    // no info: the ceiling was learned without a live measurement, so there is
    // no anchor — baseline = NO_INFO makes the first real level a re-probe.
    m_baseline = isStale() ? NO_INFO : m_strength;
    m_hasBaseline = true;
    // A new baseline starts a new run: any debounce accumulated against the OLD
    // baseline must not carry over to the new one.
    m_condTicks = 0;
}

bool NetworkReference::environmentChanged() const
{
    if (m_lastStrengthMs == 0) return false;                          // never got 603
    if ((m_clock.elapsed() - m_lastStrengthMs) > ABR_STRENGTH_STALE_MS) return false; // stale
    if (!m_hasBaseline) return false;                                 // nothing learned
    return m_condTicks >= ABR_STRENGTH_CONFIRM_TICKS;
}

void NetworkReference::reset()
{
    m_strength = NO_INFO;
    m_baseline = NO_INFO;
    m_hasBaseline = false;
    m_lastStrengthMs = 0;
    m_condTicks = 0;
    // Clock is left running: elapsed() is monotonic and needs no restart.
}
