#include "C2TelemetryPolicy.h"
#include "ABRConfigs.h"
#include <cmath>

void C2TelemetryPolicy::reset() { *this = C2TelemetryPolicy(); }
void C2TelemetryPolicy::update(const QVector<C2TelemetryStat> &samples, qint64 now)
{
    m_samples = samples;
    m_received = now;
}

C2TelemetryPolicy::Decision C2TelemetryPolicy::evaluate(qint64 now)
{
    Decision result;
    const qint64 elapsed = m_received < 0 ? C2_TELEMETRY_STALE_MS + 1 : qMax(qint64(0), now - m_received);
    auto fresh = [elapsed](bool valid, qint64 age) {
        return valid && age >= 0 && elapsed <= C2_TELEMETRY_STALE_MS &&
               age <= C2_TELEMETRY_STALE_MS - elapsed;
    };
    bool ready = !m_samples.isEmpty();
    bool signal = !m_samples.isEmpty();
    Level observed = Normal;
    for (const auto &sample : m_samples) {
        const bool fc = sample.captureAvailable && fresh(sample.telemetryValid, sample.telemetryAgeMs);
        const bool heartbeat = fresh(sample.heartbeatValid, sample.heartbeatAgeMs);
        const bool feedback = fresh(sample.peerAckValid, sample.ackAgeMs) && fresh(sample.rttValid, sample.rttAgeMs) &&
                              std::isfinite(sample.rttSmoothedMs) && sample.rttSmoothedMs > 0;
        signal = signal && fc;
        ready = ready && fc && heartbeat && feedback && sample.baselineValid && sample.observationOk;
        if (!fc || !heartbeat || !feedback || !sample.observationOk) continue;
        result.worstRttMs = qMax(result.worstRttMs, sample.rttSmoothedMs);
        // Absolute latency can trigger protection during baseline warmup.
        if (sample.rttSmoothedMs > 300 || (sample.baselineValid && sample.rttInflationMs >= 90)) observed = Critical;
        else if (sample.rttSmoothedMs > 60 || (sample.baselineValid && sample.rttInflationMs >= 30)) observed = qMax(observed, High);
        if (sample.dataPackets >= C2_MIN_PACKETS_FOR_RETRANS_RATIO && sample.retransmissionPercent >= 0) {
            if (sample.retransmissionPercent >= 8) observed = Critical;
            else if (sample.retransmissionPercent >= 2.5) observed = qMax(observed, High);
        }
        // NAK count is a report rate, not a loss percentage or unique packet count.
        if (sample.nakReportsPerSecond >= 15) observed = Critical;
        else if (sample.nakReportsPerSecond >= 3) observed = qMax(observed, High);
    }
    if (observed > m_level) {
        m_level = observed;
        m_recoverySince = -1;
        m_wasReady = false;
    }
    if (!ready) {
        m_recoverySince = -1;
    } else if (!m_wasReady || observed < m_level || m_recoverySince >= 0) {
        if (m_recoverySince < 0 || m_recoveryLevel != observed) {
            m_recoverySince = now;
            m_recoveryLevel = observed;
        }
        if (now - m_recoverySince >= C2_TELEMETRY_RECOVERY_MS) {
            m_level = observed;
            m_recoverySince = -1;
        }
    }
    m_wasReady = ready;
    result.signal = signal;
    result.inputsReady = ready;
    result.holdIncrease = !ready || m_recoverySince >= 0;
    result.level = m_level;
    if (m_level == High) result.ceilingKbps = C2_HIGH_VIDEO_CEILING_KBPS;
    if (m_level == Critical) result.ceilingKbps = C2_CRITICAL_VIDEO_CEILING_KBPS;
    return result;
}

unsigned int C2TelemetryPolicy::limit(unsigned int requested, unsigned int current, qint64 now)
{
    const auto decision = evaluate(now);
    unsigned int target = qMin(requested, decision.ceilingKbps);
    // Missing FC/capture/ACK freezes increases. Camera QoS may still reduce video.
    // Allow the existing survival floor to bootstrap while C2 is warming up.
    if (decision.holdIncrease) target = qMin(target, qMax(unsigned(ABR_MIN_ACTIVE_VIDEO_BITRATE_KBPS), current));
    return target;
}
