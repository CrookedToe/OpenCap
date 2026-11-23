#pragma once

#include "sensor_calibration.hpp"
#include <CommunityV1Calibration.h>
#include <optional>

namespace opencap {

class LiveHandCalibration {
public:
    using Clock = CommunityV1Calibration::Clock;
    LiveHandCalibration() { reset({}); }
    void reset(const SensorRange& seed);
    void setLearning(bool enabled);
    bool learning() const { return learning_; }
    const SensorRange& update(const RawGloveSample& sample, Clock::time_point time = Clock::now());
    const SensorRange& range() const { return range_; }
private:
    struct Discovery {
        std::array<int, 5> samples{};
        unsigned count{}, index{};
        int direction{}, peak{}, trough{};
        std::array<std::optional<int>, 2> lastTurn{};
        std::array<unsigned, 2> visits{};
        Clock::time_point turnTime{};
        bool observe(int raw, std::size_t channel, Clock::time_point time);
    };
    SensorRange range_{};
    bool learning_{true};
    CommunityV1Calibration tracker_;
    std::array<Discovery, 12> discovery_{};
    bool readyForSplay(const RawGloveSample& sample, Clock::time_point time);
    RawGloveSample bendReference_{};
    std::optional<Clock::time_point> steadySince_;
    std::optional<Clock::time_point> previous_;
};

}
