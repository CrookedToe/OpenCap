#include "model.hpp"

#include <cmath>
#include <numbers>

namespace opencap {
bool isLive(const Snapshot& snapshot, Side side, Millis now) {
    const auto& hand = snapshot.hands.at(static_cast<std::size_t>(side));
    return hand.linked && hand.lastFrame && now >= hand.lastFrame && now >= snapshot.heartbeat
        && now - hand.lastFrame <= sampleTimeout && now - snapshot.heartbeat <= sampleTimeout;
}

Quaternion normalize(Quaternion q) {
    const auto length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (!std::isfinite(length) || length < 1e-6f) return {};
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

Quaternion multiply(Quaternion a, Quaternion b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

Quaternion eulerDegrees(Vec3 degrees) {
    constexpr auto halfAngle = std::numbers::pi_v<float> / 360.0f;
    const auto x = degrees.x * halfAngle;
    const auto y = degrees.y * halfAngle;
    const auto z = degrees.z * halfAngle;
    return normalize(multiply({0, 0, std::sin(z), std::cos(z)},
        multiply({0, std::sin(y), 0, std::cos(y)}, {std::sin(x), 0, 0, std::cos(x)})));
}

Vec3 rotate(Quaternion rotation, Vec3 point) {
    const auto q = normalize(rotation);
    const auto r = multiply(multiply(q, {point.x, point.y, point.z, 0}), {-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}

Hand demoHand(Side side, Millis now) {
    const auto t = static_cast<float>(now % 20000) / 1000.0f
        + static_cast<float>(static_cast<std::uint32_t>(side));
    Hand hand;
    hand.lastFrame = now;
    hand.receiver = true;
    hand.linked = true;
    hand.calibrated = true;
    hand.battery = .8f;
    hand.fps = 120;
    hand.signal = -46;
    hand.channel = side == Side::left ? 20 : 50;
    hand.stickX = std::cos(t);
    hand.stickY = std::sin(t);
    for (std::size_t finger = 0; finger < hand.curl.size(); ++finger) {
        const auto curl = (std::sin(t * 2.0f + static_cast<float>(finger) * .4f) + 1.0f) / 2.0f;
        hand.curl[finger] = curl;
        for (std::size_t joint = 0; joint < 3; ++joint) {
            hand.joints[finger * 3 + joint].rotation = eulerDegrees({0, 0, -curl * 70.0f});
        }
    }
    hand.trigger = hand.curl[1];
    hand.grip = (hand.curl[2] + hand.curl[3]) / 2.0f;
    hand.pad = hand.curl[0];
    constexpr std::array mainAngle{2, 6, 10, 14, 18};
    for (std::size_t finger = 0; finger < mainAngle.size(); ++finger) {
        hand.angles[mainAngle[finger]] = -hand.curl[finger] * 65;
        hand.angles[mainAngle[finger] - 1] = -hand.curl[finger] * 55;
    }
    if (hand.trigger >= .6f) hand.buttons |= bit(Button::trigger);
    if (hand.grip >= .6f) hand.buttons |= bit(Button::grip);
    if (hand.pad >= .6f) hand.buttons |= bit(Button::pad);
    return hand;
}
}
