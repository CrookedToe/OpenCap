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
}
