#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Final camera ceiling, independent of SIYI and the active ABR engine.
class C2BitrateGuard {
public:
    struct Peer {
        std::string id;
        double rttMs;
        int64_t retrans, drops;
        bool c2Only = false;
    };
    enum class State { Waiting, Healthy, Degraded, Critical, Stale, Recovering };
    explicit C2BitrateGuard(int maximum = 4000, int floor = 250)
        : maximum(roundDown(maximum)), floor(roundDown(std::min(floor, maximum))), ceiling(this->maximum) {}
    void receive(const std::vector<Peer> &peers, int64_t now, int current) {
        if (peers.empty()) return;
        tick(now);
        rtt = retransRate = dropRate = 0;
        bool only = false;
        bool hasRateBaseline = true;
        std::map<std::string, Previous> next;
        for (const auto &p : peers) {
            if (!std::isfinite(p.rttMs) || p.rttMs < 0 || p.retrans < 0 || p.drops < 0) continue;
            rtt = std::max(rtt, p.rttMs);
            only |= p.c2Only;
            auto old = previous.find(p.id);
            const bool comparable = old != previous.end() && now > old->second.time &&
                now - old->second.time <= staleMs;
            hasRateBaseline &= comparable && p.retrans >= old->second.retrans && p.drops >= old->second.drops;
            if (comparable) {
                const double seconds = (now - old->second.time) / 1000.0;
                retransRate = std::max(retransRate, p.retrans >= old->second.retrans
                    ? (p.retrans - old->second.retrans) / seconds : 0.0);
                dropRate = std::max(dropRate, p.drops >= old->second.drops
                    ? (p.drops - old->second.drops) / seconds : 0.0);
            }
            next[p.id] = {now, p.retrans, p.drops};
        }
        if (next.empty()) return;
        previous = std::move(next);
        const bool freshSample = !seen || now > lastSample;
        lastSample = now;
        seen = true;
        const bool critical = rtt > 300 || retransRate >= 15 || dropRate > 15;
        const bool degraded = rtt > 60 || retransRate >= 1 || dropRate >= 1;
        if (freshSample) {
            criticalSamples = critical ? std::min(criticalSamples + 1, criticalConfirmSamples) : 0;
        }
        if (only || critical) {
            // Protect C2 immediately at the video floor. Cutting video needs an
            // explicit C2-only request or persistent critical measurements.
            const bool cutoff = only || ceiling == 0 || criticalSamples >= criticalConfirmSamples;
            state = cutoff ? State::Critical : State::Degraded;
            ceiling = cutoff ? 0 : std::min(ceiling, floor);
            healthySince = -1;
            degradedSamples = 0;
            recoveringOutage = true;
        } else if (degraded) {
            healthySince = -1;
            if (ceiling == 0) return;
            degradedSamples = std::min(degradedSamples + 1, degradationConfirmSamples);
            if (degradedSamples < degradationConfirmSamples) return;
            state = State::Degraded;
            recoveringOutage = false;
            if (lastReduction < 0 || now - lastReduction >= 1000) {
                const int base = std::min(ceiling, current > 0 ? current : maximum);
                ceiling = roundDown(std::min(base, std::max(floor, base * 80 / 100)));
                lastReduction = now;
            }
        } else {
            degradedSamples = 0;
            if (healthySince < 0) healthySince = now;
            state = ceiling < maximum ? State::Recovering : State::Healthy;
            // The first healthy report can resume video at the floor. A higher
            // rate requires measured retransmit/drop rates from fresh counters.
            if (recoveringOutage && ceiling == 0) {
                ceiling = floor;
                lastRecovery = now;
            }
            const bool recoveryConfirmed = recoveringOutage ? hasRateBaseline : now - healthySince >= 5000;
            const auto interval = recoveringOutage ? outageRecoveryIntervalMs : 2000;
            if (ceiling < maximum && recoveryConfirmed &&
                (lastRecovery < 0 || now - lastRecovery >= interval)) {
                const int step = recoveringOutage ? outageRecoveryStepKbps : std::max(100, ceiling / 10);
                ceiling = roundDown(ceiling + std::min(step, maximum - ceiling));
                lastRecovery = now;
            }
            if (ceiling == maximum) {
                state = State::Healthy;
                recoveringOutage = false;
            }
        }
    }
    void tick(int64_t now) {
        if (seen && now - lastSample > staleMs) {
            healthySince = -1;
            degradedSamples = 0;
            criticalSamples = 0;
            if (now - lastSample > outageMs) {
                state = State::Stale;
                ceiling = std::min(ceiling, floor);
                recoveringOutage = true;
            }
        }
    }
    bool hasFreshSample(int64_t now) const { return seen && now - lastSample <= staleMs; }
    bool isRecoveringFromOutage() const { return recoveringOutage; }
    static int roundDown(int bitrate) { return std::max(0, bitrate) / 50 * 50; }
    int clamp(int requested) const { return roundDown(std::min(requested, ceiling)); }
    const char *name() const {
        switch (state) {
        case State::Healthy: return "HEALTHY";
        case State::Degraded: return "DEGRADED";
        case State::Critical: return "CRITICAL";
        case State::Stale: return "STALE";
        case State::Recovering: return "RECOVERING";
        default: return "WAITING";
        }
    }
    int maximum, floor, ceiling;
    State state = State::Waiting;
    double rtt = 0, retransRate = 0, dropRate = 0;
    static constexpr int64_t staleMs = 3000;
    static constexpr int64_t outageMs = 5000;
    static constexpr int degradationConfirmSamples = 5;
private:
    static constexpr int criticalConfirmSamples = 3;
    static constexpr int64_t outageRecoveryIntervalMs = 1000;
    static constexpr int outageRecoveryStepKbps = 1200;
    struct Previous { int64_t time, retrans, drops; };
    std::map<std::string, Previous> previous;
    bool seen = false;
    bool recoveringOutage = false;
    int criticalSamples = 0;
    int degradedSamples = 0;
    int64_t lastSample = 0, healthySince = -1, lastReduction = -1, lastRecovery = -1;
};
