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

struct Snapshot {
    Millis heartbeat{};
    bool demo{};
    bool receiverSupported{};
    std::array<Hand, handCount> hands{};
    std::array<HandSettings, handCount> settings{};
};

enum class Action : std::uint32_t {
    startHand, fist, together, spread, cancelHand,
    joystickCenter, joystickRange, joystickFinish,
    pairStart, pairStop, pairBoth, pairBothStop, channel, haptic, toggleStick, adjustPose, setJointOffset
};

struct Command {
    Side side{};
    Action action{};
    int value{};
    float duration{};
    float amplitude{};
    float extra{};
};

bool validJointOffset(Vec3 offset);
void applyHandPreferences(Hand& hand, const HandSettings& settings);

bool isLive(const Snapshot& snapshot, Side side, Millis now);
std::uint32_t remapButtons(std::uint32_t input, const ButtonMap& map);
Quaternion normalize(Quaternion value);
Quaternion multiply(Quaternion a, Quaternion b);
Quaternion eulerDegrees(Vec3 degrees);
Vec3 rotate(Quaternion rotation, Vec3 position);
Hand demoHand(Side side, Millis now);
}
