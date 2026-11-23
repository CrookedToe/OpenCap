#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <ostream>

namespace opencap {
FingerSplay calibratedSplay(const CalibrationSamples& calibration, const RawGloveSample& sample) {
    return calibratedSplay(sensorRange(calibration), sample);
}

FingerSplay calibratedSplay(const SensorRange& range, const RawGloveSample& sample) {
    FingerSplay result{{}, true};
    for (std::size_t gap = 0; gap < splaySensorChannels.size(); ++gap) {
        const auto channel = splaySensorChannels[gap];
        const int together = range[0][channel];
        const int span = range[1][channel] - together;
        if (std::abs(span) < minimumSplaySpan) continue;
        result.gaps[gap] = std::clamp(static_cast<float>(sample[channel] - together) / span, 0.0f, 1.0f);
    }
    return result;
}

CalibrationGrade calibrationGrade(std::size_t channel, std::uint16_t movement) {
    const auto& cutoffs = sensorGradeThresholds.at(channel);
    return static_cast<CalibrationGrade>(std::ranges::upper_bound(cutoffs, movement) - cutoffs.begin());
}

const char* calibrationGradeName(CalibrationGrade grade) {
    constexpr std::array names{"Poor", "Average", "Good", "Excellent"};
    return names[static_cast<std::size_t>(grade)];
}

CalibrationSpans calibrationSpans(const CalibrationSamples& samples) {
    return calibrationSpans(sensorRange(samples));
}

CalibrationSpans calibrationSpans(const SensorRange& range) {
    CalibrationSpans result;
    result.measured = true;
    for (std::size_t i = 0; i < result.counts.size(); ++i) {
        const auto span = static_cast<std::uint16_t>(std::abs(
            static_cast<int>(range[1][i]) - range[0][i]));
        result.counts[i] = span;
        if (calibrationGrade(i, span) == CalibrationGrade::poor) ++result.lowChannels;
    }
    return result;
}

std::wstring radioSignalLabel(const Hand& hand, bool live) {
    if (!live || hand.signal >= 0) return L"No signal reading";
    const auto quality = hand.signal > -45 ? L"Strong" : hand.signal > -60 ? L"Fair" : L"Weak";
    return std::format(L"{} ({} dBm)", quality, hand.signal);
}

void writeCalibrationReport(std::ostream& output, Side side, const CalibrationSamples& samples) {
    const auto spans = calibrationSpans(samples);
    const auto hand = side == Side::left ? "left" : "right";
    output << "hand,channel_type,channel,fist,together,spread,movement_counts,quality\n";
    for (std::size_t i = 0; i < spans.counts.size(); ++i) {
        const auto span = spans.counts[i];
        output << hand << ',' << (isSplayChannel(i) ? "splay" : "flexion") << ',' << i + 4 << ','
               << samples[0][i] << ',' << samples[1][i] << ',' << samples[2][i] << ',' << span << ','
               << calibrationGradeName(calibrationGrade(i, span)) << '\n';
    }
    for (std::size_t i = spans.counts.size(); i < samples.front().size(); ++i)
        output << hand << ",auxiliary," << i - spans.counts.size() + 1 << ',' << samples[0][i] << ','
               << samples[1][i] << ',' << samples[2][i] << ",,\n";
}

bool applyTrackingPreset(std::array<HandSettings, handCount>& hands, std::size_t preset) {
    if (preset >= trackingPresets.size()) return false;
    for (std::size_t i = 0; i < hands.size(); ++i) {
        hands[i].position = trackingPresets[preset].hands[i].position;
        hands[i].rotation = trackingPresets[preset].hands[i].rotation;
    }
    return true;
}

bool validJointOffset(Vec3 offset) {
    return std::abs(offset.x) <= 45 && std::abs(offset.y) <= 45 && std::abs(offset.z) <= 45;
}

bool validButtonRange(ButtonRange range) {
    return range.touch >= 0 && range.click <= 1 && range.touch <= range.click;
}

const wchar_t* calibrationPose(unsigned phase) {
    switch (phase) {
    case 1: return L"Make a fist";
    case 2: return L"Straighten fingers and bring them together";
    case 3: return L"Keep your fingers straight and spread them apart";
    default: return L"";
    }
}

std::wstring calibrationPrompt(const CalibrationGuide& guide, Millis time) {
    if (!guide.hands) return {};
    if (!guide.captureAt) return guide.phase == 1 ? L"Starting hand calibration…" : L"Recording pose…";
    const auto remaining = time < guide.captureAt ? (guide.captureAt - time + 999) / 1000 : 0;
    if (!remaining) return L"Recording pose…";
    return std::wstring(calibrationPose(guide.phase)) + L"  •  " + std::to_wstring(remaining) + L" s";
}

bool validHandCurve(std::size_t channel, const HandCurve& curve) {
    return channel < HandCurves{}.size() && curve.min >= -180 && curve.max <= 180 && curve.min <= curve.max &&
        curve.balance >= 0 && curve.balance <= 1 && std::abs(curve.offset) <= 45 &&
        (channel != 11 || (curve.min == 0 && curve.max == 0 && curve.balance == .5));
}
bool StickHold::update(bool held, Millis time) {
    if (!held) { pressed_ = consumed_ = false; return false; }
    if (!pressed_ || time < started_) {
        started_ = time;
        pressed_ = true;
    }
    if (consumed_ || time - started_ < 5000) return false;
    consumed_ = true;
    return true;
}

void applyHandPreferences(Hand& hand, const HandSettings& settings) {
    if (!settings.triggerEnabled) { hand.trigger = 0; hand.buttons &= ~bit(Button::trigger); }
    if (!settings.gripEnabled) { hand.grip = 0; hand.buttons &= ~bit(Button::grip); }
    if (!settings.padEnabled) { hand.pad = 0; hand.buttons &= ~bit(Button::pad); }
    if (!settings.thumbEnabled) {
        hand.curl[0] = 0;
        for (const auto index : {0, 1, 2, 3, 20}) hand.angles[index] = 0;
        for (unsigned j = 0; j < 3; ++j) hand.joints[j] = {};
    }
}

std::uint32_t remapButtons(std::uint32_t input, const ButtonMap& map) {
    constexpr unsigned mappedMask = (1u << std::tuple_size_v<ButtonMap>) - 1;
    auto mapped = input & ~mappedMask;
    for (std::size_t output = 0; output < map.size(); ++output) {
        const auto source = map[output];
        if (source < disabledButton && input & (1u << source)) mapped |= 1u << output;
    }
    return mapped;
}

bool isLive(const Snapshot& snapshot, Side side, Millis now) {
    const auto& hand = snapshot.hands.at(static_cast<std::size_t>(side));
    return hand.linked && hand.lastFrame && now >= hand.lastFrame && now >= snapshot.heartbeat
        && now - hand.lastFrame <= sampleTimeout && now - snapshot.heartbeat <= sampleTimeout;
}

unsigned handsReadyForCalibration(const Snapshot& snapshot, Millis time) {
    unsigned hands = 0;
    if (!snapshot.calibrationGuide.hands)
        for (unsigned i = 0; i < handCount; ++i)
            if (!snapshot.hands[i].calibrationStep && isLive(snapshot, static_cast<Side>(i), time))
                hands |= 1u << i;
    return hands;
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
    hand.controllerRevision = side == Side::left ? 1 : 2;
    hand.battery = .8f;
    hand.fps = 120;
    hand.signal = -46;
    hand.channel = side == Side::left ? 20 : 50;
    hand.stickX = std::cos(t);
    hand.stickY = std::sin(t);
    hand.splay.measured = true;
    for (std::size_t gap = 0; gap < hand.splay.gaps.size(); ++gap)
        hand.splay.gaps[gap] = .5f + .5f * std::cos(2.0f * t + .25f * static_cast<float>(gap));
    for (std::size_t finger = 0; finger < hand.curl.size(); ++finger) {
        const auto curl = (std::sin(t * 2.0f + static_cast<float>(finger) * .4f) + 1.0f) / 2.0f;
        hand.curl[finger] = curl;
        for (std::size_t joint = 0; joint < 3; ++joint) {
            const float bend = (side == Side::left ? 1.0f : -1.0f) * curl * (70 - 8 * static_cast<float>(joint));
            hand.joints[finger * 3 + joint].rotation = eulerDegrees(finger == 0 ? Vec3{0, -bend, 0} : Vec3{0, 0, bend});
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
