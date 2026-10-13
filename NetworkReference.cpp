#include "NetworkReference.h"
#include "ABRGpsMove.h"   // haversineMeters

NetworkReference::NetworkReference()
{
    m_clock.start();
}

bool NetworkReference::isStale() const
{
    if (m_lastStrengthMs == 0) return true;              // never got a 603
    return (m_clock.elapsed() - m_lastStrengthMs) > ABR_STRENGTH_STALE_MS;
}

bool NetworkReference::gpsStale() const
{
    if (m_lastGpsMs == 0) return true;                   // never got a 604
    // Same window as strength: 1 Hz cadence, fail-closed after silence.
    return (m_clock.elapsed() - m_lastGpsMs) > ABR_STRENGTH_STALE_MS;
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

void NetworkReference::setGps(double lat, double lon)
{
    // Trust boundary: the fix arrives on the unauthenticated UDP telemetry port.
    // A lat/lon outside the valid range is coerced to "no fix" (drop the update),
    // so a rogue 604 cannot seed an anchor or fake a move. NaN fails both tests.
    if (!(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) {
        return;
    }

    m_fixLat = lat;
    m_fixLon = lon;
    m_lastGpsMs = m_clock.elapsed();

    // If the ceiling was learned with NO fix, we have no anchor. Adopt THIS fix
    // as the anchor — not an auto-lift: the first fix carries no "we moved"
    // evidence, it only gives a datum to measure FUTURE movement against.
    if (!m_gpsAnchorValid) {
        m_anchorLat = lat;
        m_anchorLon = lon;
        m_gpsAnchorValid = true;
    }
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

    // Snapshot the position too. A stale snapshot is no fix: the ceiling was
    // learned without a live fix, so there is no position anchor — m_gpsAnchorValid
    // stays false and the FIRST real fix (setGps) seeds the anchor, not a lift.
    if (m_lastGpsMs != 0 && !gpsStale()) {
        m_anchorLat = m_fixLat;
        m_anchorLon = m_fixLon;
        m_gpsAnchorValid = true;
    } else {
        m_gpsAnchorValid = false;
    }
}

bool NetworkReference::environmentChanged() const
{
    // OR of two independent escapes: the radio recovered, OR the vehicle moved.
    return strengthRecovered() || gpsMoved();
}

bool NetworkReference::strengthRecovered() const
{
    if (m_lastStrengthMs == 0) return false;                          // never got 603
    if ((m_clock.elapsed() - m_lastStrengthMs) > ABR_STRENGTH_STALE_MS) return false; // stale
    if (!m_hasBaseline) return false;                                 // nothing learned
    return m_condTicks >= ABR_STRENGTH_CONFIRM_TICKS;
}

bool NetworkReference::gpsMoved() const
{
    if (!m_gpsAnchorValid) return false;                 // learned with no fix, no datum yet
    if (gpsStale()) return false;                        // no fresh fix (fail-closed)
    return haversineMeters(m_anchorLat, m_anchorLon, m_fixLat, m_fixLon)
               >= ABR_GPS_MOVE_METERS;
}

void NetworkReference::reset()
{
    m_strength = NO_INFO;
    m_baseline = NO_INFO;
    m_hasBaseline = false;
    m_lastStrengthMs = 0;
    m_condTicks = 0;
    m_anchorLat = 0.0;
    m_anchorLon = 0.0;
    m_gpsAnchorValid = false;
    m_fixLat = 0.0;
    m_fixLon = 0.0;
    m_lastGpsMs = 0;
    // Clock is left running: elapsed() is monotonic and needs no restart.
}
