#include "radio_channel.hpp"

#include <iostream>
#include <stdexcept>

using namespace opencap;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Link {
    Hand hand;
    RadioTraffic traffic;
    RadioSelector selector;
    RadioChannels occupied;
    Millis time{100}, signalTime{100};
    bool available{true}, refreshSignal{true};

    Link() {
        hand.receiver = hand.linked = true;
        hand.signal = -35;
        hand.channel = 1;
    }
    std::optional<int> run(Millis duration, Millis interval) {
        const auto end = time + duration;
        while (time < end) {
            time += interval;
            traffic.frame(time);
            hand.lastFrame = time;
            if (refreshSignal) signalTime = time;
            if (auto channel = selector.update(time, hand, signalTime, traffic, occupied, available))
                return channel;
        }
        return {};
    }
    void confirm(int channel) {
        hand.channel = channel;
        hand.pendingChannel = -1;
        traffic.lastFrame = 0;
        selector.interruptMeasurement();
    }
};
}

int main() {
    try {
        Link link;
        check(!link.run(20000, 10), "Leave a clean link alone");
        link.occupied.set(2);
        const auto trial = link.run(13000, 100);
        check(trial == 3, "Persistent stalls initiate a trial on an unoccupied channel");
        link.hand.pendingChannel = *trial;
        check(!link.run(5000, 100), "Wait for receiver readback");
        link.confirm(*trial);
        check(!link.run(20000, 10) && !link.selector.busy(), "Keep a clean confirmed candidate");

        Link compare;
        for (int i = 4; i <= 10; ++i) compare.occupied.set(i);
        check(compare.run(12000, 100) == 2, "Try first free candidate");
        compare.confirm(2);
        check(compare.run(12000, 65) == 3, "Measure another candidate while gaps persist");
        compare.confirm(3);
        check(compare.run(12000, 120) == 2, "Return to the best measured channel");
        compare.confirm(2);
        check(!compare.run(60000, 100), "Avoid repeated trials after returning");

        Link collision;
        collision.occupied.set(1);
        collision.available = false;
        check(!collision.run(15000, 10), "Defer while the other receiver is changing");
        collision.available = true;
        check(collision.run(5000, 10) == 2, "Resolve a shared channel even on a smooth link");
        check(!collision.run(20000, 100), "Rejected change enters cooldown");
        check(collision.run(120000, 100) == 3, "Retry another channel after a rejected write");

        Link unconfirmed;
        check(unconfirmed.run(12000, 100) == 2, "Start trial before simulating lost readback");
        unconfirmed.hand.pendingChannel = 2;
        check(!unconfirmed.run(35000, 10) && !unconfirmed.selector.busy(), "Unconfirmed write times out");
        unconfirmed.hand.pendingChannel = -1;
        unconfirmed.hand.channel = -1;
        check(!unconfirmed.run(130000, 100), "An unknown radio channel cannot initiate a switch");

        Link gated;
        gated.refreshSignal = false;
        gated.signalTime = 0;
        check(!gated.run(30000, 100), "Require fresh strength telemetry");
        gated.refreshSignal = true;
        gated.hand.signal = -70;
        check(!gated.run(20000, 100), "Weak signal must not trigger a write");
        gated.hand.signal = -35;
        gated.hand.calibrationStep = 1;
        check(!gated.run(20000, 100), "Defer during calibration");
        gated.hand.calibrationStep = 0;
        gated.hand.pairing = true;
        check(!gated.run(20000, 100), "Defer during pairing");
        gated.hand.pairing = false;
        check(gated.run(12000, 100) == 2, "Resume sampling once eligible");
        gated.confirm(2);
        gated.hand.linked = false;
        check(!gated.run(35000, 100) && !gated.selector.busy(), "A lost trial releases the other receiver");
        gated.hand.linked = true;
        check(gated.run(200, 100) == 1, "Recover a timed-out trial only after a safe reconnection");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
