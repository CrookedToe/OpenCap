#include "osc_compat.hpp"

#include <algorithm>
#include <cmath>

namespace opencap {
namespace {
struct Axis {
    std::string_view control;
    unsigned channel;
    float add;
    float divisor;
    float subtract{};
};

constexpr std::array<Axis, avatarParameterCount> axes{{
    {"Thumb1", 2, 80, 80, .5f},
    {"Thumb2", 1, 30, 40},
    {"Thumb3", 0, 80, 65},
    {"Thumb1spread", 20, 45, 60},
    {"Index1", 6, 80, 90},
    {"Index2", 5, 87, 87},
    {"Index1spread", 7, 0, 20},
    {"Middle1", 10, 80, 80},
    {"Middle2", 9, 87, 87},
    {"Ring1", 14, 87, 87},
    {"Ring2", 13, 87, 87},
    {"Ring1spread", 15, 0, 12},
    {"Pinky1", 18, 75, 75},
    {"Pinky2", 17, 90, 90},
    {"Pinky1spread", 19, 0, 40}
}};
}

unsigned avatarBits(float value) {
    // First-party avatars decode four bits as the nearest reciprocal, 1/n.
    float error = 2;
    unsigned result = 0;
    for (unsigned n = 1; n <= 15; ++n) {
        const auto candidate = std::abs(value - 1.0f / static_cast<float>(n));
        if (candidate < error) {
            error = candidate;
            result = n;
        }
    }
    return result;
}

std::array<AvatarParameter, avatarParameterCount> avatarParameters(const Snapshot& sample, Side side) {
    const auto hand = static_cast<std::size_t>(side);
    const auto& angles = sample.hands.at(hand).angles;
    std::array<AvatarParameter, avatarParameterCount> result{};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto& axis = axes[i];
        // UDCAP 0.1.8.7 sends the right ring spread for both avatar parameters.
        const auto angle = axis.control == "Ring1spread" ? sample.hands[1].angles[axis.channel] : angles[axis.channel];
        const auto bias = axis.control == "Thumb1" && side == Side::right ? .82f : axis.subtract;
        result[i] = {axis.control, avatarBits((angle + axis.add) / axis.divisor - bias)};
    }
    return result;
}

std::array<Quaternion, jointCount> vmcFingerRotations(const Hand& hand, Side side) {
    const auto& a = hand.angles;
    const float bend = side == Side::left ? -1.0f : 1.0f;
    const std::array<Vec3, jointCount> angles{{
        {a[21], -bend * a[7], bend * a[6]}, {0, 0, bend * a[5]}, {0, 0, bend * a[4]},
        {0, a[11], bend * a[10]}, {0, 0, bend * a[9]}, {0, 0, bend * a[8]},
        {0, bend * a[15], bend * a[14]}, {0, 0, bend * a[13]}, {0, 0, bend * a[12]},
        {a[22], bend * a[19], bend * a[18]}, {0, 0, bend * a[17]}, {0, 0, bend * a[16]},
        {1.2f * a[3], -.3f * bend * a[2], .3f * bend * a[20]},
        {0, -bend * a[1], 0}, {0, -bend * a[0], 0}
    }};
    std::array<Quaternion, jointCount> result{};
    std::ranges::transform(angles, result.begin(), eulerDegrees);
    return result;
}

std::array<VmcBlend, 8> vmcControllerInputs(const Hand& hand) {
    const auto pressed = [&](Button button) { return hand.buttons & bit(button) ? 1.0f : 0.0f; };
    return {{
        {"JoyX_Positive", std::max(0.0f, hand.stickX)}, {"JoyX_Negative", std::max(0.0f, -hand.stickX)},
        {"JoyY_Positive", std::max(0.0f, hand.stickY)}, {"JoyY_Negative", std::max(0.0f, -hand.stickY)},
        {"Button_A", pressed(Button::a)}, {"Button_B", pressed(Button::b)},
        {"Button_Joy", pressed(Button::stick)}, {"Button_Menu", pressed(Button::system)}
    }};
}
}
