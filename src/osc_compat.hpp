#pragma once

#include "model.hpp"

#include <string_view>

namespace opencap {
struct AvatarParameter {
    std::string_view control;
    unsigned bits{};
};

constexpr std::size_t avatarParameterCount = 15;

unsigned avatarBits(float value);
std::array<AvatarParameter, avatarParameterCount> avatarParameters(const Snapshot& sample, Side side);

struct VmcBlend {
    std::string_view control;
    float value{};
};

// Index through little finger, followed by thumb. Each finger is proximal to distal.
std::array<Quaternion, jointCount> vmcFingerRotations(const Hand& hand, Side side);
std::array<VmcBlend, 8> vmcControllerInputs(const Hand& hand);
}
