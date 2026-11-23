#include "radio_channel.hpp"

namespace opencap {
void RadioTraffic::frame(Millis time) {
    constexpr Millis gapAllowance = 50;
    if (lastFrame && time > lastFrame + gapAllowance) stalled += time - lastFrame - gapAllowance;
    lastFrame = time;
}

bool RadioSelector::safeToSwitch(const Hand& hand, Millis signalTime, Millis time) const {
    return hand.receiver && hand.linked && !hand.pairing && !hand.calibrationStep &&
        hand.channel >= 0 && hand.channel <= 100 && hand.pendingChannel < 0 &&
        hand.lastFrame && time - hand.lastFrame <= sampleTimeout &&
        signalTime && time - signalTime <= 15000 && hand.signal < 0 && hand.signal > -45;
}

void RadioSelector::finish(Millis time) {
    const int lastTrial = lastTrial_;
    *this = {};
    lastTrial_ = lastTrial;
    resume_ = time + 120000;
}

std::optional<int> RadioSelector::update(Millis time, const Hand& hand, Millis signalTime,
        const RadioTraffic& traffic, const RadioChannels& occupied, bool available) {
    if (busy() && time >= deadline_) {
        const int from = target_, to = best_;
        const bool recover = !returning_ && to >= 0 && from != to;
        finish(time);
        if (recover) {
            recoveryFrom_ = from;
            recoveryTo_ = to;
        }
    }
    if (recoveryFrom_ >= 0 && available && safeToSwitch(hand, signalTime, time)) {
        const bool canReturn = hand.channel == recoveryFrom_ && !occupied.test(recoveryTo_);
        const int to = recoveryTo_;
        recoveryFrom_ = recoveryTo_ = -1;
        if (canReturn) {
            returning_ = true;
            target_ = to;
            deadline_ = time + 30000;
            return to;
        }
    }
    if (busy() && hand.pendingChannel < 0 && hand.channel != target_) {
        // The write failed or a different channel was reported. Never score it as the target.
        const int fallback = !returning_ && hand.channel == measured_ ? best_ : -1;
        finish(time);
        if (fallback >= 0 && fallback != hand.channel) {
            recoveryFrom_ = hand.channel;
            recoveryTo_ = fallback;
        }
    }
    if (busy() && hand.pendingChannel < 0 && measured_ != target_) {
        measured_ = target_;
        startFrame_ = 0;
        resume_ = time + 5000;
    }
    if (!available || !safeToSwitch(hand, signalTime, time) || time < resume_) {
        // A temporarily late frame should count when it arrives. A link break,
        // calibration or stale signal must not bridge two measurement windows.
        if (!available || !hand.linked || hand.calibrationStep || hand.pairing ||
            hand.pendingChannel >= 0 || hand.signal <= -45 || hand.signal >= 0 ||
            !signalTime || time - signalTime > 15000) {
            startFrame_ = 0;
            badWindows_ = 0;
        }
        return {};
    }
    if (returning_) {
        finish(time);
        return {};
    }
    if (!startFrame_ || measured_ != hand.channel) {
        if (measured_ != hand.channel) badWindows_ = 0;
        measured_ = hand.channel;
        startFrame_ = traffic.lastFrame;
        startStalled_ = traffic.stalled;
        return {};
    }
    const auto duration = traffic.lastFrame - startFrame_;
    if (duration < 4000) return {};
    const double score = static_cast<double>(traffic.stalled - startStalled_) / duration;
    startFrame_ = traffic.lastFrame;
    startStalled_ = traffic.stalled;
    const bool shared = occupied.test(hand.channel);
    if (!busy()) {
        badWindows_ = score >= 0.05 ? badWindows_ + 1 : 0;
        if (!shared && badWindows_ < 2) return {};
        tried_.reset();
        best_ = -1;
    }
    tried_.set(hand.channel);
    if (best_ >= 0 && occupied.test(best_)) best_ = -1;
    if (!shared && (best_ < 0 || score < bestScore_)) {
        bestScore_ = score;
        best_ = hand.channel;
    }
    // Stop interrupting tracking once stalls take less than 1% of the window.
    // Rotate the first candidate too, so a rejected write cannot starve the rest.
    if (shared || score >= 0.01) {
        const int previous = lastTrial_ ? lastTrial_ : hand.channel;
        for (int offset = 1; offset <= 10; ++offset) {
            const int channel = (previous + offset - 1) % 10 + 1;
            if (tried_.test(channel) || occupied.test(channel)) continue;
            lastTrial_ = channel;
            target_ = channel;
            deadline_ = time + 30000;
            return channel;
        }
    }
    if (best_ >= 0 && best_ != hand.channel) {
        returning_ = true;
        target_ = best_;
        deadline_ = time + 30000;
        return best_;
    }
    finish(time);
    return {};
}
}
