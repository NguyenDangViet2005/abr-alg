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
        const auto previousState = state;
        rtt = retransRate = dropRate = 0;
        bool only = false;
        std::map<std::string, Previous> next;
        for (const auto &p : peers) {
            if (!std::isfinite(p.rttMs) || p.rttMs < 0 || p.retrans < 0 || p.drops < 0) continue;
            rtt = std::max(rtt, p.rttMs);
            only |= p.c2Only;
            auto old = previous.find(p.id);
            if (old != previous.end() && now > old->second.time && now - old->second.time <= staleMs) {
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
        lastSample = now;
        seen = true;
        const bool critical = only || rtt > 300 || retransRate >= 15 || dropRate > 15;
        const bool degraded = rtt > 60 || retransRate >= 1 || dropRate >= 1;
        if (critical || degraded) {
            healthySince = -1;
            if (degradedSince < 0) degradedSince = now;
            // A brief occlusion can produce one bad sample. Wait for another
            // sample roughly one telemetry interval later before cutting video.
            if (!critical && now - degradedSince < degradationConfirmMs) return;
            state = critical ? State::Critical : State::Degraded;
            if (lastReduction < 0 || now - lastReduction >= 1000 ||
                (critical && previousState != State::Critical)) {
                const int base = std::min(ceiling, current > 0 ? current : maximum);
                ceiling = roundDown(std::min(base, only ? floor : std::max(floor, base * (critical ? 50 : 80) / 100)));
                lastReduction = now;
            }
        } else {
            degradedSince = -1;
            if (healthySince < 0) healthySince = now;
            state = ceiling < maximum ? State::Recovering : State::Healthy;
            if (ceiling < maximum && now - healthySince >= 5000 &&
                (lastRecovery < 0 || now - lastRecovery >= 2000)) {
                ceiling = roundDown(std::min(maximum, ceiling + std::max(100, ceiling / 10)));
                lastRecovery = now;
                if (ceiling == maximum) state = State::Healthy;
            }
        }
    }
    void tick(int64_t now) {
        if (seen && now - lastSample > staleMs) {
            healthySince = -1;
            degradedSince = -1;
            if (now - lastSample > outageMs) {
                state = State::Stale;
                ceiling = std::min(ceiling, floor);
            }
        }
    }
    bool hasFreshSample(int64_t now) const { return seen && now - lastSample <= staleMs; }
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
    static constexpr int64_t degradationConfirmMs = 750;
private:
    struct Previous { int64_t time, retrans, drops; };
    std::map<std::string, Previous> previous;
    bool seen = false;
    int64_t degradedSince = -1;
    int64_t lastSample = 0, healthySince = -1, lastReduction = -1, lastRecovery = -1;
};
