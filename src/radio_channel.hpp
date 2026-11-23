#pragma once

#include "model.hpp"

#include <bitset>
#include <optional>

namespace opencap {
// The receiver can report its tuned channel and glove RSSI, but has no RF scan.
// Compare equal windows of glove traffic using time lost to long frame gaps.
struct RadioTraffic {
    Millis lastFrame{}, stalled{};
    void frame(Millis time);
};

using RadioChannels = std::bitset<101>; // Includes channels reported by older configurations.
class RadioSelector {
public:
    bool busy() const { return target_ >= 0; }
    void interruptMeasurement() { startFrame_ = 0; badWindows_ = 0; }
    std::optional<int> update(Millis time, const Hand& hand, Millis signalTime,
                              const RadioTraffic& traffic, const RadioChannels& occupied, bool available);
private:
    bool safeToSwitch(const Hand& hand, Millis signalTime, Millis time) const;
    void finish(Millis time);
    int measured_{-1}, target_{-1}, best_{-1};
    int recoveryFrom_{-1}, recoveryTo_{-1};
    int lastTrial_{};
    unsigned badWindows_{};
    bool returning_{};
    double bestScore_{};
    Millis startFrame_{}, startStalled_{}, resume_{}, deadline_{};
    RadioChannels tried_;
};
}
