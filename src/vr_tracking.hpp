#pragma once

#include "model.hpp"

#include <openvr_driver.h>
#include <span>

namespace opencap {
struct TrackingSource {
    TrackerDevice device;
    vr::ETrackedControllerRole role{vr::TrackedControllerRole_Invalid};
    bool tracker{};
    bool valid{};
};

class TrackerBinding {
public:
    std::uint32_t resolve(Side side, const HandSettings& settings, std::span<const TrackingSource> sources);

private:
    std::uint32_t requestedIndex_{noTracker}, selectionRevision_{};
    std::array<char, 100> requestedSerial_{}, lockedSerial_{};
};

vr::DriverPose_t makeTrackedPose(const vr::TrackedDevicePose_t* tracker, const HandSettings& settings,
                               bool connected, double sampleTimeOffset);
}
