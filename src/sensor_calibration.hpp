#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace opencap {
using RawGloveSample = std::array<std::int16_t, 19>;
inline std::int64_t sensorTimeUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
struct SensorFrame {
    RawGloveSample values{};
    unsigned count{};
    std::int64_t timeUs{};
};
using CalibrationSamples = std::array<RawGloveSample, 3>;
using SensorRange = std::array<std::array<std::int16_t, 12>, 2>; // Together, then bent/spread.
inline constexpr std::array splaySensorChannels{4u, 7u, 10u};
inline constexpr int minimumSplaySpan = 26;
constexpr bool isSplayChannel(std::size_t channel) {
    return channel == splaySensorChannels[0] || channel == splaySensorChannels[1] || channel == splaySensorChannels[2];
}
constexpr int minimumSensorSpan(std::size_t channel) {
    return isSplayChannel(channel) ? minimumSplaySpan : 10;
}
// Minimum movement for Average, Good, and Excellent in the UDCAP 0.1.8.7 hand viewer.
inline constexpr std::array<std::array<std::uint16_t, 3>, 12> sensorGradeThresholds{{
    {420, 700, 1050}, {210, 350, 700}, {130, 220, 350}, {200, 350, 700},
    {130, 220, 350}, {150, 250, 500}, {230, 380, 800}, {130, 220, 350},
    {150, 250, 500}, {200, 350, 700}, {130, 220, 350}, {300, 500, 950}
}};

inline SensorRange sensorRange(const CalibrationSamples& poses) {
    SensorRange range{};
    for (std::size_t i = 0; i < range[0].size(); ++i) {
        const bool splay = isSplayChannel(i);
        range[0][i] = poses[1][i];
        range[1][i] = poses[splay ? 2 : 0][i];
    }
    return range;
}
}
