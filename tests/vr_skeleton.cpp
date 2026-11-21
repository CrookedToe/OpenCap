#include "vr_skeleton.hpp"

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
}

int main() {
    try {
        for (const auto side : {Side::left, Side::right}) {
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
