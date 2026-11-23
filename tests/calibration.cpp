#include <CorePref.h>
#include <UdCapV1Core.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using Sample = UdCapV1Core::CalibrationSamples::value_type;
constexpr auto glove = "ACSTDE4FG540004L";
constexpr auto spare = "ACSTDE4FG540005L";

void require(bool result, const char* description) {
    if (!result) throw std::runtime_error(description);
}

void send(PortAccessor& port, CommandType command, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> packet{170, 85, 1, static_cast<std::uint8_t>(command), 0,
                                    static_cast<std::uint8_t>(data.size())};
    for (auto byte : data) packet.push_back(byte ^ 129);
    std::uint8_t crc{};
    for (std::size_t i = 2; i < packet.size(); ++i) crc += packet[i];
    packet.push_back(crc);
    port.injectReceivedData(packet);
}

void frame(PortAccessor& port, const std::span<const std::int16_t> sample) {
    std::vector<std::uint8_t> data;
    for (const auto value : sample) {
        const auto counts = static_cast<std::uint16_t>(value);
        data.push_back(static_cast<std::uint8_t>(counts >> 8));
        data.push_back(static_cast<std::uint8_t>(counts));
    }
    send(port, CMD_DATA, data);
}

void connect(PortAccessor& port, std::string_view serial) {
    port.notifyConnectionState(PORT_ACCESSOR_CONNECTION_STATE_CONNECTED);
    std::vector<std::uint8_t> link{1};
    link.insert(link.end(), serial.begin(), serial.end());
    send(port, CMD_LINK_STATE, link);
    frame(port, Sample{});
}

using Writes = std::vector<std::vector<std::uint8_t>>;

std::shared_ptr<PortAccessor> port(Writes* writes = nullptr) {
    auto result = std::make_shared<PortAccessor>();
    ManualSerialDriverCallbacks callbacks;
    callbacks.write = [writes](const std::uint8_t* data, std::size_t count) {
        if (writes) writes->emplace_back(data, data + count);
        return count;
    };
    result->bindSerialCallbacks(callbacks);
    return result;
}

void calibrate(UdCapV1Core& core, PortAccessor& transport, const UdCapV1Core::CalibrationSamples& samples) {
    core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
    constexpr std::array poses{UDCAP_V1_HAND_CALI_TYPE_FIST, UDCAP_V1_HAND_CALI_TYPE_ADDUCTION,
                               UDCAP_V1_HAND_CALI_TYPE_PROTRACT};
    for (std::size_t i = 0; i < poses.size(); ++i) {
        frame(transport, samples[i]);
        core.captureCalibrationData(poses[i]);
    }
    core.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
}

void checkController(const UdCapV1Core::CalibrationSamples& captures) {
    std::mutex mutex;
    std::condition_variable changes;
    bool sawV2Axis = false, sawV2Button = false;
    bool sawSystemPress = false, sawSystemRelease = false;
    Writes writes;
    auto transport = port(&writes);
    UdCapV1Core core(transport);
    core.listen([&](const auto& packet) {
        std::lock_guard lock(mutex);
        if (packet->commandType == CMD_INPUT_JOYSTICK &&
            std::abs(packet->joystickData.joyX - .3f) < .001f && packet->joystickData.joyY == 0) sawV2Axis = true;
        if (packet->commandType == CMD_INPUT_BUTTON && packet->button.btnB &&
            packet->button.btnJoyStick && !packet->button.btnA && !packet->button.btnMenu) sawV2Button = true;
        if (packet->commandType == CMD_INPUT_BUTTON && packet->button.btnA && !packet->button.btnB &&
            !packet->button.btnJoyStick && !packet->button.btnPower) {
            if (packet->button.btnMenu) sawSystemPress = true;
            else if (sawSystemPress) sawSystemRelease = true;
        }
        changes.notify_all();
    });
    connect(*transport, glove);
    const std::vector<std::uint8_t> startStream{85, 170, 1, 1, 3, 1, 65, 67, 138};
    require(std::ranges::find(writes, startStream) != writes.end(),
            "Request the consumer glove's combined input stream");
    calibrate(core, *transport, captures);
    core.mcuSendHaptic(.125f, .8f, 140);
    require(writes.back() == std::vector<std::uint8_t>{85, 170, 1, 4, 4, 3, 0, 12, 8, 32},
            "Route legacy haptics to the glove's vibration command");
    require(core.getControllerRevision() == 1, "Report Controller 1.0 from the input frame");
    core.mcuTestHaptic(false);
    core.mcuTestHaptic(true);
    require(writes[writes.size() - 2] == std::vector<std::uint8_t>{85, 170, 1, 4, 4, 2, 0, 10, 10, 31} &&
            writes.back() == std::vector<std::uint8_t>{85, 170, 1, 4, 4, 1, 0, 10, 10, 30},
            "Test the two Controller 1.0 motors independently");
    std::array<std::int16_t, 15> compact{};
    compact[12] = 2600;
    compact[13] = 2000;
    compact[14] = 128 + 4 + 1;
    frame(*transport, compact);
    {
        std::unique_lock lock(mutex);
        require(changes.wait_for(lock, std::chrono::seconds(2), [&] { return sawV2Axis && sawV2Button; }),
                "Decode the Controller 2.0 joystick and buttons from the compact frame");
    }
    frame(*transport, std::span(compact).first(14));
    frame(*transport, std::span(compact).first(12));
    require(core.getControllerRevision() == 2, "Short sensor frames preserve the detected Controller 2.0 revision");
    compact[14] = 0x80 | 0x10 | 0x02;
    frame(*transport, compact);
    compact[14] = 0x80 | 0x02;
    frame(*transport, compact);
    {
        std::unique_lock lock(mutex);
        require(changes.wait_for(lock, std::chrono::seconds(2), [&] { return sawSystemRelease; }),
                "Controller 2.0 System presses and releases independently of the A and power buttons");
    }
    core.mcuSendHaptic(.125f, .8f, 140);
    require(writes.back() == std::vector<std::uint8_t>{85, 170, 1, 0xce, 8, 2, 0, 0x98, 3, 140, 0, 125, 0, 125},
            "Encode Controller 2.0 haptics with independent strength, frequency and time");
    require(core.getControllerRevision() == 2, "Report Controller 2.0 from a compact input frame");
    core.mcuTestHaptic(false);
    core.mcuTestHaptic(true);
    require(writes[writes.size() - 2] == std::vector<std::uint8_t>{85, 170, 1, 0xce, 8, 2, 0, 88, 2, 100, 0, 232, 3, 130} &&
            writes.back() == std::vector<std::uint8_t>{85, 170, 1, 0xce, 8, 2, 0, 232, 3, 100, 0, 232, 3, 19},
            "Use the Controller 2.0 low and high actuator patterns");
    frame(*transport, compact);
    core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_JOYSTICK);
    core.captureJoystickData(UDCAP_V1_JOYSTICK_CALI_TYPE_CENTER);
    core.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_JOYSTICK);
    for (std::uint8_t step = 1; step <= 3; ++step) {
        const std::vector<std::uint8_t> expected{85, 170, 1, 0xcc, 1, step, static_cast<std::uint8_t>(0xce + step)};
        require(writes[writes.size() - 4 + step] == expected, "Send Controller 2.0 calibration actions in order");
    }
    send(*transport, CMD_LINK_STATE, {0});
    send(*transport, CMD_LINK_STATE, {1});
    frame(*transport, std::span(compact).first(12));
    require(core.getControllerRevision() == 0, "An unannounced controller does not inherit the previous link's revision");
    core.mcuTestHaptic(false);
    require(writes.back() == std::vector<std::uint8_t>{85, 170, 1, 4, 4, 2, 0, 10, 10, 31},
            "A linked glove can run the legacy motor test before controller identification");
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() / ("OpenCap-calibration-" +
                      std::to_string(GetCurrentProcessId()) + ".json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    try {
        UdCapV1Core::CalibrationSamples captures{};
        captures[0][0] = -154;
        captures[1][0] = 200;
        captures[1][13] = 850;
        captures[2][4] = 456;
        checkController(captures);
        {
            auto transport = port();
            UdCapV1Core core(transport);
            connect(*transport, glove);
            core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
            core.tryRestoreHandCalibration();
            require(core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_NONE,
                    "Cancel the first calibration");
            calibrate(core, *transport, captures);
            require(core.handCalibrationSamples() == captures, "Keep the exact poses captured by the core");
            core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
            core.tryRestoreHandCalibration();
            require(core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED &&
                    core.handCalibrationSamples() == captures, "Cancel recalibration and restore previous results");
            bool rejected = false;
            try { calibrate(core, *transport, {}); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected && core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED &&
                    core.handCalibrationSamples() == captures, "Preserve the last good calibration after a failed attempt");
            auto curves = core.getHandResponse();
            curves[6] = {-70, 10, .65, -5};
            core.setHandResponse(curves);
            require(core.savePref(path.string()), "Save calibration using the normal glove profile");
        }
        {
            auto transport = port();
            UdCapV1Core core(transport);
            connect(*transport, glove);
            require(core.loadPref(path.string()) && core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED &&
                    core.handCalibrationSamples() == captures, "Restore calibration in a new core instance");
            const auto adjustment = core.getHandResponse()[6];
            require(adjustment.outputMin == -70 && adjustment.outputMax == 10 &&
                    adjustment.balance == .65 && adjustment.offset == -5,
                    "Restore an edited finger response curve in a new core instance");
            send(*transport, CMD_LINK_STATE, {0});
            connect(*transport, glove);
            core.tryRestoreHandCalibration();
            require(core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED &&
                    core.handCalibrationSamples() == captures, "Restore calibration after a radio reconnect");
            connect(*transport, spare);
            require(core.loadPref(path.string()) && !core.handCalibrationSamples() &&
                    core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_NONE,
                    "Keep a profile's calibration tied to its original glove");
        }
        {
            auto profile = nlohmann::json{};
            {
                std::ifstream input(path);
                input >> profile;
            }
            profile["handCalibration"]["poses"][0][0] = std::uint64_t(-1);
            {
                std::ofstream output(path);
                output << profile;
            }
            auto transport = port();
            UdCapV1Core core(transport);
            connect(*transport, glove);
            require(core.loadPref(path.string()) && !core.handCalibrationSamples(),
                    "Reject an invalid calibration while retaining readable settings");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
