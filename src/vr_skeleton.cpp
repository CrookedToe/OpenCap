#include "vr_skeleton.hpp"

#include <hand_simulation.h>

#include <algorithm>
#include <cmath>
#include <numbers>

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

float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

Vec3 unit(Vec3 v) {
    const auto length = std::sqrt(dot(v, v));
    return length > 1e-6f ? Vec3{v.x/length, v.y/length, v.z/length} : Vec3{};
}

Quaternion around(Vec3 unitAxis, float degrees) {
    const auto half = degrees * std::numbers::pi_v<float> / 360;
    const auto s = std::sin(half);
    return {unitAxis.x*s, unitAxis.y*s, unitAxis.z*s, std::cos(half)};
}

Quaternion inverse(Quaternion q) { return {-q.x, -q.y, -q.z, q.w}; }

Quaternion toBoneRotation(Quaternion sensor, Side side, bool thumb) {
    const bool right = side == Side::right;
    // Both OpenVR reference hands bend around local Z. UDCAP encodes thumb
    // flexion around Y and flips the sign of flexion between hands.
    if (thumb) return normalize(right ? Quaternion{sensor.x, -sensor.z, sensor.y, sensor.w}
                                    : Quaternion{sensor.x, sensor.z, -sensor.y, sensor.w});
    return normalize(right ? Quaternion{sensor.x, -sensor.y, -sensor.z, sensor.w} : sensor);
}

struct NeutralHand {
    VrHandSkeleton bones;
    Vec3 forward, across, spreadAxis;
};

NeutralHand makeNeutralHand(Side side) {
    NeutralHand neutral{};
    auto& bones = neutral.bones;
    MyHandSimulation reference;
    const auto role = side == Side::left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
    reference.ComputeSkeletonTransforms(role, {}, {}, bones.data());
    // Preserve the wrist in the grip frame. The SteamVR pose aligns that frame to the hand-mounted tracker.

    const auto shaft = [&](std::size_t finger) {
        const auto proximal = firstJoint[finger];
        const auto q = multiply(rotation(bones[proximal - 1]), rotation(bones[proximal]));
        return unit(rotate(q, position(bones[proximal + 1])));
    };
    const auto knuckle = [&](std::size_t finger) {
        const auto proximal = firstJoint[finger];
        const auto base = position(bones[proximal - 1]);
        const auto vector = rotate(rotation(bones[proximal - 1]), position(bones[proximal]));
        return Vec3{base.x+vector.x, base.y+vector.y, base.z+vector.z};
    };
    neutral.forward = shaft(2);
    neutral.spreadAxis = unit(cross(neutral.forward, subtract(knuckle(4), knuckle(1))));
    neutral.across = unit(cross(neutral.spreadAxis, neutral.forward));
    for (std::size_t finger = 1; finger < firstJoint.size(); ++finger) {
        const auto proximal = firstJoint[finger];
        const auto parentRotation = rotation(bones[proximal - 1]);
        const auto from = shaft(finger);
        const auto axis = cross(from, neutral.forward);
        const auto close = normalize({axis.x, axis.y, axis.z, 1 + dot(from, neutral.forward)});
        setRotation(bones[proximal], multiply(inverse(parentRotation), multiply(close,
                    multiply(parentRotation, rotation(bones[proximal])))));
    }
    return neutral;
}

const NeutralHand& neutralHand(Side side) {
    static const auto neutral = std::array{makeNeutralHand(Side::left), makeNeutralHand(Side::right)};
    return neutral[static_cast<std::size_t>(side)];
}

Quaternion removeModeledSplay(const Hand& hand, Side side, std::size_t finger, Quaternion sensor) {
    const auto angle = hand.angles[splayCurveChannels[finger - 1]];
    const float mirror = side == Side::left ? -1.0f : 1.0f;
    const auto yaw = (finger == 1 ? -mirror : finger == 2 ? 1.0f : mirror) * angle;
    const auto pitch = hand.jointOffsets[finger*3].x + (finger == 1 ? hand.angles[21] : finger == 4 ? hand.angles[22] : 0);
    // The receiver composes local Z, Y, X. Remove its modeled Y angle while keeping joint corrections and bend.
    const auto x = eulerDegrees({pitch, 0, 0});
    return multiply(sensor, multiply(inverse(x), multiply(eulerDegrees({0, -yaw, 0}), x)));
}

std::array<float, 5> fingerSpread(const Hand& hand, Side side) {
    const auto spread = [&](std::size_t finger, std::size_t gap, float nominal) {
        const auto& curve = hand.splayCurves[finger];
        const auto level = hand.splay.gaps[gap];
        const auto shaped = level <= .5f ? std::lerp(0.0, curve.balance, 2.0*level)
                                        : std::lerp(curve.balance, 1.0, 2.0*level - 1);
        return static_cast<float>(nominal * (std::lerp(curve.min, curve.max, shaped) + curve.offset) /
                                  defaultSplayCurves[finger].max);
    };
    const auto middle = static_cast<float>(hand.splayCurves[1].offset) * (side == Side::left ? -1 : 1);
    const auto ring = spread(2, 1, 14);
    // The three measurements are adjacent gaps. Anchor the fan at the middle finger.
    return {0, middle - spread(0, 0, 18), middle, middle + ring, middle + ring + spread(3, 2, 18)};
}

auto resolveBoneTransforms(const VrHandSkeleton& poses, const vr::VRBoneTransform_t& wrist) {
    std::array<vr::VRBoneTransform_t, parent.size()> rootSpace{poses[0], wrist};
    for (std::size_t i = 2; i < parent.size(); ++i) {
        const auto& basis = rootSpace[parent[i]];
        const auto q = rotation(basis);
        const auto offset = rotate(q, position(poses[i]));
        const auto p = position(basis);
        rootSpace[i].position = {{p.x + offset.x, p.y + offset.y, p.z + offset.z, 1}};
        setRotation(rootSpace[i], multiply(q, rotation(poses[i])));
    }
    return rootSpace;
}
}

VrHandSkeleton makeVrHandSkeleton(Side side, const Hand& hand) {
    const auto& reference = neutralHand(side);
    auto bones = reference.bones;

    if (hand.calibrated) {
        const auto spread = fingerSpread(hand, side);
        for (std::size_t finger = 0; finger < firstJoint.size(); ++finger) {
            for (unsigned joint = 0; joint < 3; ++joint) {
                auto& bone = bones[firstJoint[finger] + joint];
                auto sensor = hand.joints[finger*3 + joint].rotation;
                auto base = rotation(bone);
                if (hand.splay.measured && finger > 0 && joint == 0) {
                    sensor = removeModeledSplay(hand, side, finger, sensor);
                    const auto parentRotation = rotation(bones[firstJoint[finger] - 1]);
                    const auto axis = rotate(inverse(parentRotation), reference.spreadAxis);
                    base = multiply(around(axis, spread[finger]), base);
                }
                setRotation(bone, multiply(base, toBoneRotation(sensor, side, finger == 0)));
            }
        }
    }
    // The auxiliary bones are children of the root, so their knuckle poses are in root space.
    const auto rootSpace = resolveBoneTransforms(bones, bones[eBone_Wrist]);
    for (std::size_t finger = 0; finger < lastKnuckle.size(); ++finger)
        bones[eBone_Aux_Thumb + finger] = rootSpace[lastKnuckle[finger]];
    return bones;
}

HandPreview makeHandPreview(Side side, const Hand& hand) {
    const auto& neutral = neutralHand(side);
    const auto bones = makeVrHandSkeleton(side, hand);
    const auto wristSpace = resolveBoneTransforms(bones, bones[eBone_Root]);
    const auto mirror = side == Side::left ? 1.0f : -1.0f;
    HandPreview preview{};
    for (std::size_t finger = 0; finger < preview.size(); ++finger) {
        const auto first = firstJoint[finger];
        // The thumb begins at its metacarpal. The other fingers have a separate palm bone.
        const auto start = finger == 0 ? first : first - 1;
        for (std::size_t joint = start; joint <= first + 3; ++joint) {
            const auto point = position(wristSpace[joint]);
            preview[finger][joint - start + (finger == 0 ? 1 : 0)] = {
                mirror * dot(point, neutral.across), dot(point, neutral.forward), mirror * dot(point, neutral.spreadAxis)
            };
        }
    }
    return preview;
}
}
