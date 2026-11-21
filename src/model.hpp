#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opencap {
using Millis = std::uint64_t;
constexpr Millis sampleTimeout = 500;
constexpr std::size_t handCount = 2;
constexpr std::size_t jointCount = 15;
constexpr std::uint32_t noTracker = UINT32_MAX;

enum class Side : std::uint32_t { left, right };
enum class Button : std::uint32_t {
    a = 1u << 0, b = 1u << 1, system = 1u << 2, stick = 1u << 3,
    power = 1u << 4, trigger = 1u << 5, grip = 1u << 6, pad = 1u << 7
};

constexpr std::uint32_t bit(Button value) { return static_cast<std::uint32_t>(value); }
using ButtonMap = std::array<unsigned, 4>;
constexpr unsigned disabledButton = 8;

struct Vec3 { float x{}, y{}, z{}; };
struct Quaternion { float x{}, y{}, z{}, w{1}; };
struct Joint { Quaternion rotation{}; };
struct ButtonRange {
    float touch{}, click{};
    bool operator==(const ButtonRange&) const = default;
};
using ButtonRanges = std::array<ButtonRange, 3>;
constexpr ButtonRanges defaultButtonRanges{{{.25f, .5f}, {.5f, 1}, {.5f, 1}}};
using RawGloveSample = std::array<std::int16_t, 19>;
struct CalibrationSpans {
    std::array<std::uint16_t, 12> counts{};
    unsigned lowChannels{};
    bool measured{};
};
CalibrationSpans calibrationSpans(const std::array<RawGloveSample, 3>& samples);

class StickHold {
public:
    bool update(bool held, Millis time);

private:
    Millis started_{};
    bool pressed_{}, consumed_{};
};

struct Hand {
    Millis lastFrame{};
    float fps{};
    float battery{-1};
    int signal{};
    int channel{-1};
    bool receiver{};
    bool linked{};
    bool calibrated{};
    bool pairing{};
    unsigned calibrationStep{};
    unsigned calibrationRequests{};
    std::uint32_t buttons{};
    float stickX{}, stickY{}, trigger{}, grip{}, pad{};
    ButtonRanges buttonRanges{defaultButtonRanges};
    CalibrationSpans quality{};
    std::array<float, 5> curl{};
    std::array<float, 23> angles{};
    std::array<Joint, jointCount> joints{};
    std::array<Vec3, jointCount> jointOffsets{};
    std::array<char, 40> firmware{};
    std::array<char, 96> message{};
};

struct HandSettings {
    std::uint32_t tracker{noTracker};
    std::array<char, 100> trackerSerial{};
    Vec3 position{};
    Vec3 rotation{};
    ButtonMap buttonMap{0, 1, 2, 3};
    bool enabled{true};
    bool joystick{true};
    bool haptics{true};
    bool highPriority{};
    bool triggerEnabled{true}, gripEnabled{true}, padEnabled{true}, thumbEnabled{true};
    bool skeletonOnly{};
    float poseTimeOffset{};
};

struct TrackerAlignment { Vec3 position, rotation; };
struct TrackingPreset {
    const wchar_t* name;
    std::array<TrackerAlignment, handCount> hands;
};
inline constexpr std::array trackingPresets{
    TrackingPreset{L"VIVE Tracker", {{{{.1f, .1f, .05f}, {45, -85, 0}}, {{-.1f, .1f, .05f}, {45, 85, 0}}}}},
    TrackingPreset{L"Tundra Tracker", {{{{-.09f, .09f, .04f}, {60, -60, 75}}, {{-.09f, -.09f, .04f}, {60, 60, 105}}}}},
    TrackingPreset{L"Quest controller", {{{{.1f, -.1f, -.02f}, {-35, -20, 0}}, {{-.1f, -.1f, -.02f}, {-35, 20, 0}}}}},
    TrackingPreset{L"VIVE Ultimate Tracker", {{{{.1f, -.05f, -.06f}, {46, 13, -88}}, {{.09f, -.07f, .07f}, {220, 11, 80}}}}}
};
bool applyTrackingPreset(std::array<HandSettings, handCount>& hands, std::size_t preset);

struct Snapshot {
    Millis heartbeat{};
    bool demo{};
    bool receiverSupported{};
    std::array<Hand, handCount> hands{};
    std::array<HandSettings, handCount> settings{};
};

struct TrackerDevice {
    std::uint32_t index{noTracker};
    std::array<char, 100> serial{};
};

struct TrackingSnapshot {
    Millis heartbeat{};
    std::array<std::uint32_t, handCount> selected{noTracker, noTracker};
    std::array<TrackerDevice, 63> devices{};
    unsigned count{};
};

inline bool trackingCurrent(const TrackingSnapshot& status, Millis time) {
    return status.heartbeat && time >= status.heartbeat && time - status.heartbeat < 2000;
}

enum class Action : std::uint32_t {
    startHand, fist, together, spread, cancelHand,
    joystickCenter, joystickRange, joystickFinish,
    pairStart, pairStop, pairBoth, pairBothStop, channel, haptic, toggleStick, adjustPose, setJointOffset,
    setButtonRanges, applyPreset, guidedCalibration
};

struct Command {
    Side side{};
    Action action{};
    int value{};
    float duration{};
    float amplitude{};
    float extra{};
    ButtonRanges ranges{};
};

bool validJointOffset(Vec3 offset);
bool validButtonRange(ButtonRange range);
void applyHandPreferences(Hand& hand, const HandSettings& settings);

bool isLive(const Snapshot& snapshot, Side side, Millis now);
std::uint32_t remapButtons(std::uint32_t input, const ButtonMap& map);
Quaternion normalize(Quaternion value);
Quaternion multiply(Quaternion a, Quaternion b);
Quaternion eulerDegrees(Vec3 degrees);
Vec3 rotate(Quaternion rotation, Vec3 position);
Hand demoHand(Side side, Millis now);
}
