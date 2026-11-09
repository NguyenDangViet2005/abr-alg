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
    g.receive({{"a", 80, 501, 100}}, 2000, 4000);
    assert(g.ceiling == 4000); // One bad sample is an occlusion candidate.
    g.receive({{"a", 80, 501, 100}}, 3000, 4000);
    assert(g.ceiling == 3200);
    assert(g.clamp(4000) == 3200 && g.clamp(2000) == 2000 && g.clamp(0) == 0);
    g.receive({{"a", 400, 516, 100}}, 3100, 3200);
    assert(g.ceiling == 1600 && g.state == Guard::State::Critical);
    g.receive({{"a", 400, 516, 100}}, 3200, 1600);
    assert(g.ceiling == 1600); // Burst cannot repeatedly squeeze camera.
    g.receive({{"a", 10, 0, 0}}, 4000, 1600);
    assert(g.retransRate == 0 && g.dropRate == 0); // Counter reset.
    for (int t = 5000; t <= 8000; t += 1000) g.receive({{"a", 10, 0, 0}}, t, 1600);
    assert(g.ceiling == 1600); // Five seconds of healthy samples required.
    g.receive({{"a", 10, 0, 0}}, 9000, 1600);
    assert(g.ceiling == 1750);
    g.receive({{"a", 10, 0, 0}}, 10000, 1750);
    assert(g.ceiling == 1750); // Recovery rate limited.
    g.tick(13001);
    assert(g.ceiling == 1750); // Short telemetry gap holds the current ceiling.
    g.tick(15001);
    assert(g.ceiling == 250 && g.state == Guard::State::Stale);
    g.receive({{"a", 10, 900, 900}}, 16000, 250);
    assert(g.ceiling == 250 && g.retransRate == 0); // Fresh baseline after outage.
    Guard multi;
    multi.receive({{"a", 10, 0, 0}, {"b", 350, 0, 0}}, 0, 4000);
    assert(multi.ceiling == 2000); // Worst peer protects control link.
    multi.receive({{"new", 10, 999, 999}}, 1000, 2000);
    assert(multi.retransRate == 0); // New peer does not inherit old counters.
    Guard rates;
    rates.receive({{"a", 10, 0, 0}}, 0, 4000);
    rates.receive({{"a", 10, 2, 0}}, 2000, 4000);
    assert(rates.retransRate == 1 && rates.ceiling == 4000);
    rates.receive({{"a", 10, 3, 0}}, 3000, 4000);
    assert(rates.retransRate == 1 && rates.ceiling == 3200);
    rates.receive({{"a", 400, 3, 0}}, 3100, 3200);
    assert(rates.ceiling == 1600); // Critical overrides degradation cooldown.
    for (int t = 4100; t <= 13100; t += 1000)
        rates.receive({{"a", 400, 3, 0}}, t, rates.ceiling);
    assert(rates.ceiling == 250); // Persistent deterioration reaches survival.
    Guard low;
    low.receive({{"a", 400, 0, 0}}, 0, 50);
    assert(low.ceiling == 50); // Protection cannot raise an already lower video.
    Guard blink;
    blink.receive({{"a", 10, 0, 0}}, 0, 4000);
    blink.receive({{"a", 80, 1, 0}}, 1000, 4000);
    blink.receive({{"a", 10, 1, 0}}, 2000, 4000);
    blink.tick(6000);
    blink.receive({{"a", 10, 20, 10}}, 6000, 4000);
    assert(blink.ceiling == 4000 && blink.state == Guard::State::Healthy);
    assert(blink.retransRate == 0 && blink.dropRate == 0);
    blink.receive({{"a", 80, 20, 10}}, 7000, 4000);
    assert(blink.ceiling == 4000); // Healthy sample reset confirmation.
    Guard gap;
    gap.receive({{"a", 400, 0, 0}}, 0, 4000);
    gap.receive({{"a", 10, 0, 0}}, 1000, 2000);
    gap.receive({{"a", 10, 0, 0}}, 10000, 2000);
    assert(gap.ceiling == 250); // Outage cannot count toward healthy recovery.
    Guard rounded;
    assert(rounded.clamp(2246) == 2200);
    assert(rounded.clamp(0) == 0);
    rounded.receive({{"a", 400, 0, 0}}, 0, 2717);
    assert(rounded.ceiling == 1350 && rounded.clamp(4000) == 1350);
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
