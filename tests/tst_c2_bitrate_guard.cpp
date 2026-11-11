#include "../C2BitrateGuard.h"
#include <cassert>
#include <iostream>

int main() {
    using Guard = C2BitrateGuard;
    Guard g;
    g.tick(10000);
    assert(g.clamp(4000) == 4000); // No observed C2 deployment.
    g.receive({{"a", 10, 500, 100}}, 0, 4000);
    assert(g.state == Guard::State::Healthy); // Historical totals are a baseline.
    g.receive({{"a", 10, 500, 100}}, 1000, 4000);
    assert(g.ceiling == 4000);
    for (int t = 2000; t <= 5000; t += 1000) {
        g.receive({{"a", 80, 500, 100}}, t, 4000);
        assert(g.ceiling == 4000);
    }
    g.receive({{"a", 80, 500, 100}}, 6000, 4000);
    assert(g.ceiling == 3200 && g.state == Guard::State::Degraded);
    assert(g.clamp(4000) == 3200 && g.clamp(2000) == 2000 && g.clamp(0) == 0);
    g.receive({{"a", 400, 500, 100}}, 6100, 3200);
    assert(g.ceiling == 0 && g.state == Guard::State::Critical);
    assert(g.clamp(4000) == 0);
    for (int t = 7000; t <= 11000; t += 1000)
        g.receive({{"a", 10, 0, 0}}, t, 0);
    assert(g.retransRate == 0 && g.dropRate == 0); // Counter reset.
    assert(g.ceiling == 0);
    g.receive({{"a", 80, 0, 0}}, 12000, 0); // Interrupt recovery.
    for (int t = 13000; t <= 17000; t += 1000) {
        g.receive({{"a", 10, 0, 0}}, t, 0);
        assert(g.ceiling == 0);
    }
    g.receive({{"a", 10, 0, 0}}, 18000, 0);
    assert(g.ceiling == 250 && g.state == Guard::State::Recovering);
    g.receive({{"a", 10, 0, 0}}, 19000, 250);
    assert(g.ceiling == 250); // Recovery rate limited.
    g.receive({{"a", 10, 0, 0}}, 20000, 250);
    assert(g.ceiling == 350);
    g.tick(23001);
    assert(g.ceiling == 350); // Short telemetry gap holds the current ceiling.
    g.tick(25001);
    assert(g.ceiling == 250 && g.state == Guard::State::Stale);
    Guard multi;
    multi.receive({{"a", 10, 0, 0}, {"b", 350, 0, 0}}, 0, 4000);
    assert(multi.ceiling == 0); // Worst peer protects control link.
    multi.receive({{"new", 10, 999, 999}}, 1000, 2000);
    assert(multi.retransRate == 0); // New peer does not inherit old counters.
    Guard rates;
    rates.receive({{"a", 10, 0, 0}}, 0, 4000);
    rates.receive({{"a", 10, 2, 0}}, 2000, 4000);
    assert(rates.retransRate == 1 && rates.ceiling == 4000);
    rates.receive({{"a", 10, 17, 0}}, 3000, 4000);
    assert(rates.retransRate == 15 && rates.ceiling == 0);
    Guard drops;
    drops.receive({{"a", 10, 0, 0}}, 0, 4000);
    drops.receive({{"a", 10, 0, 16}}, 1000, 4000);
    assert(drops.dropRate == 16 && drops.ceiling == 0);
    Guard only;
    only.receive({{"a", 10, 0, 0, true}}, 0, 4000);
    assert(only.ceiling == 0);
    Guard blink;
    blink.receive({{"a", 10, 0, 0}}, 0, 4000);
    blink.receive({{"a", 80, 1, 0}}, 1000, 4000);
    blink.receive({{"a", 10, 1, 0}}, 2000, 4000);
    blink.tick(6000);
    blink.receive({{"a", 10, 20, 10}}, 6000, 4000);
    assert(blink.ceiling == 4000 && blink.state == Guard::State::Healthy);
    assert(blink.retransRate == 0 && blink.dropRate == 0);
    for (int t = 7000; t <= 10000; t += 1000) {
        blink.receive({{"a", 80, 20, 10}}, t, 4000);
        assert(blink.ceiling == 4000); // Healthy sample reset confirmation.
    }
    blink.receive({{"a", 80, 20, 10}}, 11000, 4000);
    assert(blink.ceiling == 3200);
    Guard gap;
    gap.receive({{"a", 400, 0, 0}}, 0, 4000);
    gap.receive({{"a", 10, 0, 0}}, 1000, 2000);
    gap.receive({{"a", 10, 0, 0}}, 10000, 2000);
    assert(gap.ceiling == 0); // Outage cannot count toward healthy recovery.
    Guard rounded;
    assert(rounded.clamp(2246) == 2200);
    assert(rounded.clamp(0) == 0);
    rounded.receive({{"a", 400, 0, 0}}, 0, 2717);
    assert(rounded.ceiling == 0 && rounded.clamp(4000) == 0);
    for (int t = 1000; t < 100000; t += 1000) {
        rounded.receive({{"a", 10, 0, 0}}, t, rounded.ceiling);
        assert(rounded.ceiling % 50 == 0);
        assert(rounded.clamp(2988) % 50 == 0);
        assert(rounded.clamp(2988) <= 2988);
    }
    assert(rounded.ceiling == 4000);
    Guard invalid;
    invalid.receive({{"a", NAN, 0, 0}}, 0, 4000);
    invalid.tick(5000);
    assert(invalid.state == Guard::State::Waiting);
    std::cout << "C2 bitrate guard tests passed\n";
}
