#include "vr_tracking.hpp"

#include <algorithm>
#include <cmath>

namespace opencap {
namespace {
Quaternion matrixRotation(const vr::HmdMatrix34_t& pose) {
    const auto& m = pose.m;
    const auto trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const auto scale = 2 * std::sqrt(trace + 1);
        return normalize({(m[2][1]-m[1][2])/scale, (m[0][2]-m[2][0])/scale,
                          (m[1][0]-m[0][1])/scale, scale/4});
    }
    unsigned i = 0;
    if (m[1][1] > m[i][i]) i = 1;
    if (m[2][2] > m[i][i]) i = 2;
    const auto j = (i + 1) % 3;
    const auto k = (j + 1) % 3;
    const auto scale = 2 * std::sqrt((std::max)(0.0f, 1 + m[i][i] - m[j][j] - m[k][k]));
    if (scale < 1e-6f) return {};
    float v[3]{};
    v[i] = scale/4;
    v[j] = (m[j][i] + m[i][j])/scale;
    v[k] = (m[k][i] + m[i][k])/scale;
    return normalize({v[0], v[1], v[2], (m[k][j] - m[j][k])/scale});
}
}

std::uint32_t TrackerBinding::resolve(Side side, const HandSettings& settings, std::span<const TrackingSource> sources) {
    if (requestedIndex_ != settings.tracker || requestedSerial_ != settings.trackerSerial ||
        selectionRevision_ != settings.trackerSelectionRevision) {
        requestedIndex_ = settings.tracker;
        requestedSerial_ = settings.trackerSerial;
        selectionRevision_ = settings.trackerSelectionRevision;
        lockedSerial_ = requestedIndex_ == noTracker ? requestedSerial_ : decltype(lockedSerial_){};
    }
    if (lockedSerial_[0]) {
        const auto device = std::ranges::find_if(sources, [&](const auto& source) { return source.device.serial == lockedSerial_; });
        return device != sources.end() && device->valid ? device->device.index : noTracker;
    }
    if (requestedIndex_ != noTracker) {
        const auto device = std::ranges::find_if(sources, [&](const auto& source) { return source.device.index == requestedIndex_; });
        if (device == sources.end()) return noTracker;
        lockedSerial_ = device->device.serial;
        return device->valid ? device->device.index : noTracker;
    }
    const auto role = side == Side::left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
    const TrackingSource* candidate = nullptr;
    for (const auto& source : sources) {
        if (source.role == role && (!candidate || (source.tracker && !candidate->tracker))) candidate = &source;
    }
    if (!candidate) return noTracker;
    lockedSerial_ = candidate->device.serial;
    return candidate->valid ? candidate->device.index : noTracker;
}

vr::DriverPose_t makeTrackedPose(const vr::TrackedDevicePose_t* tracker, const HandSettings& settings,
                               bool connected, double sampleTimeOffset) {
    vr::DriverPose_t output{};
    output.qWorldFromDriverRotation.w = output.qDriverFromHeadRotation.w = output.qRotation.w = 1;
    output.deviceIsConnected = connected;
    output.result = connected ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Uninitialized;
    if (!connected || !tracker || !tracker->bDeviceIsConnected || !tracker->bPoseIsValid ||
        tracker->eTrackingResult == vr::TrackingResult_Fallback_RotationOnly) return output;

    // Keep the tracked origin and its velocities together. SteamVR predicts there and then
    // applies the tracker-local transform to the controller pose on the back of the hand.
    const auto& matrix = tracker->mDeviceToAbsoluteTracking;
    const auto orientation = matrixRotation(matrix);
    const auto alignment = eulerDegrees(settings.rotation);
    output.qRotation = {orientation.w, orientation.x, orientation.y, orientation.z};
    output.qDriverFromHeadRotation = {alignment.w, alignment.x, alignment.y, alignment.z};
    output.vecDriverFromHeadTranslation[0] = settings.position.x;
    output.vecDriverFromHeadTranslation[1] = settings.position.y;
    output.vecDriverFromHeadTranslation[2] = settings.position.z;
    for (unsigned axis = 0; axis < 3; ++axis) {
        output.vecPosition[axis] = matrix.m[axis][3];
        output.vecVelocity[axis] = tracker->vVelocity.v[axis];
        output.vecAngularVelocity[axis] = tracker->vAngularVelocity.v[axis];
    }
    output.poseTimeOffset = sampleTimeOffset;
    output.poseIsValid = true;
    output.result = tracker->eTrackingResult;
    return output;
}
}
