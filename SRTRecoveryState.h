#ifndef SRTRECOVERYSTATE_H
#define SRTRECOVERYSTATE_H

#include <algorithm>
#include <cstdint>
#include "SRTTuning.h"

// Transport-independent recovery memory. A commanded bitrate alone is not
// evidence of capacity: fresh traffic must carry at least 80% of that rate.
class SRTRecoveryState
{
public:
    static constexpr std::int64_t StableDurationMs = SRTTuning::RECOVERY_STABLE_DURATION_MS;
    static constexpr std::int64_t MemoryLifetimeMs = SRTTuning::RECOVERY_MEMORY_LIFETIME_MS;
    static constexpr unsigned int MaxStepKbps = SRTTuning::RF_RECOVERY_MAX_STEP_KBPS;
    static constexpr unsigned int StepPercent = SRTTuning::RF_RECOVERY_STEP_PERCENT;

    static unsigned int step(unsigned int current, unsigned int target, bool firstRfUpgradeStep = false)
    {
        if (target <= current) return 0;
        const unsigned int gap = target - current;
        const auto percent = firstRfUpgradeStep ? SRTTuning::RF_UPGRADE_FIRST_STEP_PERCENT : StepPercent;
        const auto maxStep = firstRfUpgradeStep ? SRTTuning::RF_UPGRADE_FIRST_MAX_STEP_KBPS : MaxStepKbps;
        const auto proportional = static_cast<unsigned int>(
            static_cast<std::uint64_t>(gap) * percent / 100);
        return std::min(gap, std::min(maxStep, std::max(SRTTuning::RF_RECOVERY_MIN_STEP_KBPS, proportional)));
    }

    static bool carriesTarget(unsigned int current, double sendKbps)
    {
        return current > 0 && sendKbps >= current * SRTTuning::RECOVERY_TARGET_LOAD_FACTOR;
    }

    static bool carriesIncrease(unsigned int previous, unsigned int target, double sendKbps)
    {
        // For small probes, 80% of the new rate can be below the OLD rate.
        // Require actual load growth as well, not just the existing traffic.
        const double progress = previous + (target > previous ? (target - previous) * SRTTuning::RECOVERY_INCREASE_PROGRESS_FACTOR : 0.0);
        return carriesTarget(target, sendKbps) && sendKbps >= progress;
    }

    void observe(std::int64_t now, unsigned int current, double sendKbps, bool healthy)
    {
        if (m_provenAt > 0 && now - m_provenAt > MemoryLifetimeMs) {
            m_proven = 0;
            m_provenAt = 0;
        }
        if (!healthy || !carriesTarget(current, sendKbps)) {
            m_candidate = 0;
            m_candidateSince = 0;
            return;
        }
        if (m_pendingTarget == current && now > m_pendingSince &&
            carriesIncrease(m_pendingPrevious, current, sendKbps)) {
            m_pendingTarget = 0;
            m_pendingPrevious = 0;
        }
        if (awaitingFeedback()) {
            m_candidate = 0;
            m_candidateSince = 0;
            return;
        }
        if (m_candidate != current) {
            m_candidate = current;
            m_candidateSince = now;
        } else if (now - m_candidateSince >= StableDurationMs && current >= m_proven) {
            m_proven = current;
            m_provenAt = now;
        }
    }

    unsigned int provenTarget(std::int64_t now) const
    {
        return m_provenAt > 0 && now >= m_provenAt && now - m_provenAt <= MemoryLifetimeMs
            ? m_proven : 0;
    }

    void bitrateChanged(std::int64_t now, unsigned int previous, unsigned int target)
    {
        m_candidate = 0;
        m_candidateSince = 0;
        m_pendingTarget = target > previous ? target : 0;
        m_pendingPrevious = target > previous ? previous : 0;
        m_pendingSince = now;
    }

    bool awaitingFeedback() const { return m_pendingTarget > 0; }
    bool feedbackTimedOut(std::int64_t now) const
    {
        return awaitingFeedback() && now - m_pendingSince >= SRTTuning::RECOVERY_FEEDBACK_TIMEOUT_MS;
    }

    void clearPending()
    {
        m_pendingTarget = 0;
        m_pendingPrevious = 0;
        m_candidate = 0;
        m_candidateSince = 0;
    }
    unsigned int rollbackTarget() const { return m_pendingPrevious; }

private:
    unsigned int m_proven = 0;
    std::int64_t m_provenAt = 0;
    unsigned int m_candidate = 0;
    std::int64_t m_candidateSince = 0;
    unsigned int m_pendingTarget = 0;
    unsigned int m_pendingPrevious = 0;
    std::int64_t m_pendingSince = 0;
};

#endif
