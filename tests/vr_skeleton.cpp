#include "vr_skeleton.hpp"
#include "vr_tracking.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace opencap;

namespace {
bool near(float left, float right) { return std::abs(left - right) < 1e-5f; }

bool sameRotation(const vr::HmdQuaternionf_t& l, const vr::HmdQuaternionf_t& r) {
    const auto dot = l.w*r.w + l.x*r.x + l.y*r.y + l.z*r.z;
    return near(std::abs(dot), 1);
}

bool samePosition(const vr::HmdVector4_t& l, const vr::HmdVector4_t& r) {
    return near(l.v[0], r.v[0]) && near(l.v[1], r.v[1]) && near(l.v[2], r.v[2]);
}

void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }

Quaternion rotation(const vr::VRBoneTransform_t& bone) {
    const auto q = bone.orientation;
    return {q.x, q.y, q.z, q.w};
}

float separation(const VrHandSkeleton& bones, unsigned left, unsigned right) {
    const auto direction = [&](unsigned joint) {
        const auto q = multiply(rotation(bones[joint-1]), rotation(bones[joint]));
        return rotate(q, {bones[joint+1].position.v[0] > 0 ? 1.0f : -1.0f, 0, 0});
    };
    const auto a = direction(left), b = direction(right);
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

void checkSplay(Side side) {
    CalibrationSamples calibration{};
    calibration[1][4] = 100; calibration[2][4] = 300;
    calibration[1][7] = 300; calibration[2][7] = 100;
    calibration[1][10] = 200; calibration[2][10] = 250;
    Hand hand;
    hand.calibrated = true;
    hand.splay = calibratedSplay(calibration, calibration[1]);
    const auto together = makeVrHandSkeleton(side, hand);
    require(near(separation(together, 7, 12), 1) && near(separation(together, 12, 17), 1) &&
            near(separation(together, 17, 22), 1), "Together calibration makes all four finger shafts parallel");
    const float wristX = side == Side::left ? -.034038f : .034038f;
    require(samePosition(together[1].position, {wristX, .036503f, .164722f, 1}),
            "The wrist joint has a fixed position in the hand's grip frame");

    hand.splay = calibratedSplay(calibration, calibration[2]);
    const auto spread = makeVrHandSkeleton(side, hand);
    require(hand.splay.gaps == std::array{1.0f,1.0f,1.0f} && near(separation(spread, 7, 22), std::cos(34.0f*3.14159265f/180)),
            "Spread calibration reaches the bounded fan with positive or negative sensor polarity");
    const auto openPreview = makeHandPreview(side, hand);
    const auto previewSpan = [](const HandPreview& points) { return std::abs(points[1].back().x - points[4].back().x); };
    hand.splayCurves[0].max = 12.5;
    const auto narrowed = makeVrHandSkeleton(side, hand);
    require(near(separation(narrowed, 7, 22), std::cos(28.0f*3.14159265f/180)), "Respect a custom splay range");
    auto sample = calibration[1];
    sample[4] = -100; sample[7] = 200; sample[10] = 1000;
    require(calibratedSplay(calibration, sample).gaps == std::array{0.0f,.5f,1.0f}, "Clamp splay beyond the captures");
    calibration[2][10] = 210;
    require(calibratedSplay(calibration, sample).gaps[2] == 0, "Ignore noisy calibration with negligible movement");

    hand.splay = calibratedSplay(calibration, calibration[1]);
    require(previewSpan(makeHandPreview(side, hand)) < previewSpan(openPreview), "Preview widens as the SteamVR fingers spread");
    hand.angles[7] = 50;
    hand.angles[21] = 12;
    hand.joints[3].rotation = eulerDegrees({12, side == Side::left ? 50.0f : -50.0f, 0});
    const auto debiased = makeVrHandSkeleton(side, hand);
    require(near(separation(debiased, 7, 12), 1), "Default modeled abduction cannot add to the zero-splay pose");
}

void checkTracking() {
    using namespace vr;
    HandSettings settings{};
    TrackerBinding binding;
    std::array sources{
        TrackingSource{{1, {'C'}}, TrackedControllerRole_LeftHand, false, true},
        TrackingSource{{2, {'T'}}, TrackedControllerRole_LeftHand, true, true}
    };
    require(binding.resolve(Side::left, settings, sources) == 2, "Prefer the mounted tracker over a same-role controller");
    sources[1].valid = false;
    require(binding.resolve(Side::left, settings, sources) == noTracker, "Never switch sources during tracking loss");
    require(binding.resolve(Side::left, settings, std::span(sources).first(1)) == noTracker,
            "Keep the binding when the tracker disconnects");
    sources[1].device.index = 3;
    sources[1].valid = true;
    require(binding.resolve(Side::left, settings, sources) == 3, "Restore the same tracker by serial at a new SteamVR index");
    ++settings.trackerSelectionRevision;
    require(binding.resolve(Side::left, settings, std::span(sources).first(1)) == 1, "Explicit automatic selection can rebind the hand");

}

void checkMountedTracker(Side side) {
    using namespace vr;
    const bool left = side == Side::left;
    HandSettings settings;
    settings.position = {-.09f, left ? .09f : -.09f, .06f};
    settings.rotation = {60, left ? -80.0f : 80.0f, left ? 75.0f : 105.0f};
    settings.poseTimeOffset = -.01f;
    const auto wrist = makeVrHandSkeleton(side, Hand{})[1].position;
    const Vec3 wristInGrip{wrist.v[0], wrist.v[1], wrist.v[2]};
    const auto gripToWrist = rotate(eulerDegrees(settings.rotation), wristInGrip);
    const Vec3 wristInTracker{settings.position.x + gripToWrist.x, settings.position.y + gripToWrist.y,
                             settings.position.z + gripToWrist.z};
    require(std::abs(std::hypot(wristInTracker.x, wristInTracker.y, wristInTracker.z) - .073f) < .001f,
            "Legacy mount settings retain the previously aligned tracker-to-wrist distance");

    TrackedDevicePose_t tracker{};
    tracker.bDeviceIsConnected = tracker.bPoseIsValid = true;
    tracker.eTrackingResult = TrackingResult_Running_OK;
    for (const auto angle : {0.0f, .4f, -.7f, 1.57f, 3.14f}) {
        const auto c = std::cos(angle), s = std::sin(angle);
        const Vec3 rotated{c*wristInTracker.x - s*wristInTracker.y, s*wristInTracker.x + c*wristInTracker.y, wristInTracker.z};
        // The tracker on the hand follows a circular path around a stationary physical wrist.
        tracker.mDeviceToAbsoluteTracking = {{{c,-s,0,1-rotated.x}, {s,c,0,2-rotated.y}, {0,0,1,3-rotated.z}}};
        tracker.vAngularVelocity.v[2] = angle == 3.14f ? 0 : 4;
        tracker.vVelocity.v[0] = tracker.vAngularVelocity.v[2] * rotated.y;
        tracker.vVelocity.v[1] = -tracker.vAngularVelocity.v[2] * rotated.x;
        const auto sample = makeTrackedPose(&tracker, settings, true, -.002);
        require(sample.poseIsValid && sample.poseTimeOffset == -.002, "Use the sampled tracker time, including submission delay");
        for (unsigned axis = 0; axis < 3; ++axis) {
            require(sample.vecPosition[axis] == tracker.mDeviceToAbsoluteTracking.m[axis][3] &&
                    sample.vecVelocity[axis] == tracker.vVelocity.v[axis] &&
                    sample.vecAngularVelocity[axis] == tracker.vAngularVelocity.v[axis],
                    "Predict from the actual tracker origin using its native velocity");
        }
        const auto fromOpenVr = [](const auto& q) -> Quaternion {
            return {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
        };
        const auto sourceRotation = fromOpenVr(sample.qRotation);
        const auto gripRotation = multiply(sourceRotation, fromOpenVr(sample.qDriverFromHeadRotation));
        const auto offset = rotate(sourceRotation, {static_cast<float>(sample.vecDriverFromHeadTranslation[0]),
            static_cast<float>(sample.vecDriverFromHeadTranslation[1]), static_cast<float>(sample.vecDriverFromHeadTranslation[2])});
        const auto jointOffset = rotate(gripRotation, wristInGrip);
        require(near(static_cast<float>(sample.vecPosition[0]) + offset.x + jointOffset.x, 1) &&
                near(static_cast<float>(sample.vecPosition[1]) + offset.y + jointOffset.y, 2) &&
                near(static_cast<float>(sample.vecPosition[2]) + offset.z + jointOffset.z, 3),
                "The tracker-local grip and grip-local wrist compose rigidly through twists and stops");
    }
    tracker.eTrackingResult = TrackingResult_Fallback_RotationOnly;
    require(!makeTrackedPose(&tracker, settings, true, 0).poseIsValid, "A rotation-only source cannot provide a wrist position");
}
}

int main() {
    try {
        checkTracking();
        for (const auto side : {Side::left, Side::right}) {
            checkMountedTracker(side);
            checkSplay(side);
            const auto neutral = makeVrHandSkeleton(side, Hand{});
            Hand sample{};
            sample.calibrated = true;
            const float direction = side == Side::left ? 1.0f : -1.0f;
            sample.joints[5].rotation = eulerDegrees({0, 0, direction * 38});
            const auto bent = makeVrHandSkeleton(side, sample);
            require(sameRotation(neutral[8].orientation, bent[8].orientation), "Distal flexion preserves proximal bones");
            require(!sameRotation(neutral[9].orientation, bent[9].orientation), "Distal flexion changes its own joint");
            require(!samePosition(neutral[10].position, {0, 0, 0, 1}) &&
                    samePosition(neutral[10].position, bent[10].position), "The fingertip preserves its local length");
            require(sameRotation(neutral[13].orientation, bent[13].orientation), "Independent digits preserve their flexion");
            require(!sameRotation(neutral[27].orientation, bent[27].orientation) &&
                     samePosition(neutral[27].position, bent[27].position), "The auxiliary follows the last knuckle");
            for (const auto& bone : bent) {
                const auto q = bone.orientation;
                require(sameRotation(q, q), "Every bone quaternion is normalized");
                require(near(bone.position.v[3], 1), "Every bone position is homogeneous");
            }

            sample.joints[5].rotation = {std::numeric_limits<float>::infinity(), 0, 0, 0};
            const auto invalid = makeVrHandSkeleton(side, sample);
            require(sameRotation(neutral[9].orientation, invalid[9].orientation),
                    "A malformed sample cannot invalidate SteamVR's skeleton");
            sample.joints[5].rotation = eulerDegrees({0, 0, direction * 70});
            sample.calibrated = false;
            const auto waiting = makeVrHandSkeleton(side, sample);
            require(sameRotation(neutral[9].orientation, waiting[9].orientation), "Uncalibrated hands use the reference pose");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
