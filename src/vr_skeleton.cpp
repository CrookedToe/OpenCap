#include "vr_skeleton.hpp"

#include <hand_simulation.h>

namespace opencap {
namespace {
static_assert(eBone_Count == std::tuple_size_v<VrHandSkeleton>);

constexpr std::array firstJoint{
    eBone_Thumb0, eBone_IndexFinger1, eBone_MiddleFinger1, eBone_RingFinger1, eBone_PinkyFinger1
};
constexpr std::array lastKnuckle{
    eBone_Thumb2, eBone_IndexFinger3, eBone_MiddleFinger3, eBone_RingFinger3, eBone_PinkyFinger3
};

constexpr std::array<unsigned, 26> parent{
    0, 0, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14,
    1, 16, 17, 18, 19, 1, 21, 22, 23, 24
};

Quaternion rotation(const vr::VRBoneTransform_t& bone) {
    const auto q = bone.orientation;
    return {q.x, q.y, q.z, q.w};
}

Vec3 position(const vr::VRBoneTransform_t& bone) {
    const auto& p = bone.position.v;
    return {p[0], p[1], p[2]};
}

void setRotation(vr::VRBoneTransform_t& bone, Quaternion q) {
    q = normalize(q);
    bone.orientation = {q.w, q.x, q.y, q.z};
}

Quaternion toBoneRotation(Quaternion sensor, Side side, bool thumb) {
    const bool right = side == Side::right;
    // Both OpenVR reference hands bend around local Z. UDCAP encodes thumb
    // flexion around Y and flips the sign of flexion between hands.
    if (thumb) return normalize(right ? Quaternion{sensor.x, -sensor.z, sensor.y, sensor.w}
                                    : Quaternion{sensor.x, sensor.z, -sensor.y, sensor.w});
    return normalize(right ? Quaternion{sensor.x, -sensor.y, -sensor.z, sensor.w} : sensor);
}

void updateAuxiliary(VrHandSkeleton& poses) {
    // Auxiliary bones are direct children of the root. Build the knuckle
    // transforms in root space before assigning them to the auxiliaries.
    std::array<vr::VRBoneTransform_t, parent.size()> rootSpace{};
    rootSpace[0] = {{{0, 0, 0, 1}}, {1, 0, 0, 0}};
    for (std::size_t i = 1; i < parent.size(); ++i) {
        const auto& basis = rootSpace[parent[i]];
        const auto q = rotation(basis);
        const auto offset = rotate(q, position(poses[i]));
        const auto p = position(basis);
        rootSpace[i].position = {{p.x + offset.x, p.y + offset.y, p.z + offset.z, 1}};
        setRotation(rootSpace[i], multiply(q, rotation(poses[i])));
    }
    for (std::size_t f = 0; f < lastKnuckle.size(); ++f)
        poses[eBone_Aux_Thumb + f] = rootSpace[lastKnuckle[f]];
}
}

VrHandSkeleton makeVrHandSkeleton(Side side, const Hand& hand) {
    VrHandSkeleton bones{};
    MyHandSimulation reference;
    const auto role = side == Side::left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
    reference.ComputeSkeletonTransforms(role, {}, {}, bones.data());

    if (hand.calibrated) {
        for (std::size_t finger = 0; finger < firstJoint.size(); ++finger) {
            for (unsigned joint = 0; joint < 3; ++joint) {
                auto& bone = bones[firstJoint[finger] + joint];
                const auto delta = toBoneRotation(hand.joints[finger * 3 + joint].rotation, side, finger == 0);
                setRotation(bone, multiply(rotation(bone), delta));
            }
        }
    }
    updateAuxiliary(bones);
    return bones;
}
}
