#include "live_calibration.hpp"

#include <algorithm>
#include <cmath>

namespace opencap {
namespace {
std::int16_t counts(double value) {
    return static_cast<std::int16_t>(std::clamp(std::lround(value), long(INT16_MIN), long(INT16_MAX)));
}
}

bool LiveHandCalibration::Discovery::observe(int raw, std::size_t channel, Clock::time_point time) {
    using namespace std::chrono_literals;
    if (count && time - turnTime > 30s) *this = {};
    if (!count) turnTime = time;
    samples[index++ % samples.size()] = raw;
    const bool firstWindow = count < samples.size();
    if (firstWindow && ++count < samples.size()) return false;
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const int value = sorted[2];
    if (firstWindow) peak = trough = value;
    peak = std::max(peak, value);
    trough = std::min(trough, value);
    const int average = sensorGradeThresholds[channel][0];
    const int minimum = std::max(minimumSensorSpan(channel), average / 4);
    const bool leftLow = direction <= 0 && value - trough >= minimum;
    const bool leftHigh = direction >= 0 && peak - value >= minimum;
    if (!leftLow && !leftHigh) return false;
    const unsigned end = leftHigh ? 1 : 0;
    const int extreme = leftHigh ? peak : trough;
    const bool firstTurn = direction == 0;
    direction = leftHigh ? -1 : 1;
    const int tolerance = std::max(minimum / 4, (peak - trough) / 6);
    peak = trough = value;
    turnTime = time;
    // The posture in which a hand finished opening is not a splay endpoint.
    // The first lateral movement starts a stroke. Only later reversals count.
    if (firstTurn && isSplayChannel(channel)) return false;
    const bool agrees = lastTurn[end] && std::abs(extreme - *lastTurn[end]) <= tolerance;
    visits[end] = agrees ? std::min(visits[end] + 1, 4u) : 1;
    lastTurn[end] = extreme;
    if (!lastTurn[0] || !lastTurn[1]) return false;
    const int excursion = *lastTurn[1] - *lastTurn[0];
    // Weak sensors can still contribute. Require more repeatable visits before
    // treating a smaller excursion as the channel's initial full scale.
    const unsigned required = excursion >= average ? 2 : 4;
    return excursion >= minimum && visits[0] >= required && visits[1] >= required;
}

void LiveHandCalibration::reset(const SensorRange& seed) {
    range_ = seed;
    discovery_ = {};
    previous_.reset();
    steadySince_.reset();
    CommunityV1Calibration::Values low{}, high{};
    for (std::size_t i = 0; i < low.size(); ++i) {
        const int span = seed[1][i] - seed[0][i];
        if (std::abs(span) < minimumSensorSpan(i)) {
            range_[0][i] = range_[1][i] = 0;
            continue;
        }
        const int sign = span < 0 ? -1 : 1;
        low[i] = sign * seed[0][i];
        high[i] = sign * seed[1][i];
    }
    tracker_.reset(low, high);
}

void LiveHandCalibration::setLearning(bool enabled) {
    if (learning_ == enabled) return;
    learning_ = enabled;
    reset(range_);
}

bool LiveHandCalibration::readyForSplay(const RawGloveSample& sample, Clock::time_point time) {
    using namespace std::chrono_literals;
    unsigned bent = 0, reliable = 0, moving = 0;
    for (std::size_t i = 0; i < range_[0].size(); ++i) {
        if (isSplayChannel(i)) continue;
        const int span = range_[1][i] - range_[0][i];
        const int typical = sensorGradeThresholds[i][0];
        if (std::abs(span) >= typical / 2) {
            ++reliable;
            if (double(sample[i] - range_[0][i]) / span > .35) ++bent;
        }
        // Raw movement also guards a new glove before bend endpoints exist.
        const int band = std::max({24, typical / 5, std::abs(span) / 6});
        if (std::abs(sample[i] - bendReference_[i]) > band) ++moving;
    }
    if (!steadySince_ || (bent && 2 * bent >= reliable) || moving >= (reliable >= 3 ? 2u : 1u)) {
        bendReference_ = sample;
        steadySince_ = time;
        for (const auto i : splaySensorChannels) discovery_[i] = {};
        return false;
    }
    return time - *steadySince_ >= 350ms;
}

const SensorRange& LiveHandCalibration::update(const RawGloveSample& sample, Clock::time_point time) {
    using namespace std::chrono_literals;
    if (!learning_) return range_;
    if (previous_ && time <= *previous_) return range_;
    if (previous_ && time - *previous_ > 500ms) reset(range_);
    previous_ = time;
    const bool observeSplay = readyForSplay(sample, time);
    CommunityV1Calibration::Values raw{};
    std::array<int, 12> signs{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        int span = range_[1][i] - range_[0][i];
        if (isSplayChannel(i)) {
            auto& stroke = discovery_[i];
            if (!observeSplay || !stroke.observe(sample[i], i, time)) continue;
            const int low = *stroke.lastTurn[0], high = *stroke.lastTurn[1];
            if (high - low < .7 * std::abs(span)) continue;
            const std::array target{span < 0 ? high : low, span < 0 ? low : high};
            for (unsigned end = 0; end < 2; ++end) {
                auto& current = range_[end][i];
                const int limit = std::abs(span) / 5;
                current = span ? counts(current + std::clamp(target[end] - current, -limit, limit))
                               : static_cast<std::int16_t>(target[end]);
            }
            stroke = {};
            continue;
        }
        if (std::abs(span) < minimumSensorSpan(i)) {
            auto& pending = discovery_[i];
            if (!pending.observe(sample[i], i, time)) continue;
            const int low = *pending.lastTurn[0], high = *pending.lastTurn[1];
            tracker_.resetChannel(i, low, high);
            span = high - low;
        }
        signs[i] = span < 0 ? -1 : 1;
        raw[i] = signs[i] * sample[i];
    }
    const auto bounds = tracker_.updateBounds(raw, time);
    for (std::size_t i = 0; i < signs.size(); ++i) {
        if (!signs[i]) continue;
        // Keep the last usable mapping if two estimated endpoints converge.
        if (bounds.high[i] - bounds.low[i] < minimumSensorSpan(i)) continue;
        range_[0][i] = counts(signs[i] * bounds.low[i]);
        range_[1][i] = counts(signs[i] * bounds.high[i]);
    }
    return range_;
}
}
