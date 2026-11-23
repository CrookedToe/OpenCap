#include <CorePref.h>
#include "calibration_log.hpp"
#include "model.hpp"
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
        const auto captured = core.captureCalibrationData(poses[i]);
        require(captured.timeUs > 0 && captured.count == samples[i].size() && captured.values == samples[i],
                "Capture the exact timestamped raw frame used by the solver");
    }
    core.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
}

void checkLiveRanges() {
    using namespace std::chrono_literals;
    using opencap::LiveHandCalibration;
    LiveHandCalibration live;
    LiveHandCalibration::Clock::time_point time{};
    Sample sample{};
    auto hold = [&](int bend, int spread, int frames) {
        sample[0] = static_cast<int16_t>(bend);
        sample[4] = static_cast<int16_t>(spread);
        for (int i = 0; i < frames; ++i) live.update(sample, time += 10ms);
    };
    hold(1500, 900, 100);
    hold(32000, 32000, 1);
    hold(1500, 900, 100);
    for (int i = 0; i < 8; ++i) { hold(1540, 915, 10); hold(1500, 900, 10); }
    require(live.range() == opencap::SensorRange{}, "A resting hand, jitter and an isolated spike cannot teach full range");
    hold(2250, 900, 30);
    hold(1500, 900, 30);
    time += 2s;
    hold(2250, 900, 30);
    hold(1500, 900, 30);
    require(live.range()[1][0] == 0, "Discard incomplete motion evidence across a frame gap");
    for (int i = 0; i < 3; ++i) { hold(2250, 900, 30); hold(1500, 900, 30); }
    require(live.range()[0][0] == 1500 && live.range()[1][0] == 2250 && !live.range()[1][4],
            "Bend learns independently while splay stays still");
    for (int i = 0; i < 4; ++i) { hold(1500, 1220, 30); hold(1500, 900, 30); }
    require(live.range()[0][4] == 900 && live.range()[1][4] == 1220, "Learn splay during later use");
    hold(1875, 1060, 600);
    require(live.range()[0][0] == 1500 && live.range()[1][0] == 2250, "A held half-grip keeps its range");
    auto seed = live.range();
    std::swap(seed[0][4], seed[1][4]);
    live.reset(seed);
    for (int i = 0; i < 5; ++i) { hold(2440, 1310, 60); hold(1690, 990, 60); }
    require(std::abs(live.range()[0][0] - 1690) < 20 && std::abs(live.range()[1][0] - 2440) < 20 &&
            live.range()[0][4] == seed[0][4] && live.range()[1][4] == seed[1][4],
            "Fist cycles adjust bend without moving either splay endpoint");
    for (int i = 0; i < 12; ++i) { hold(1690, 1310, 40); hold(1690, 990, 40); }
    require(live.range()[0][4] == 1310 && live.range()[1][4] == 990,
            "Separate spread and return strokes update a reversed saved gap");
    live.reset({});
    auto flex = [&] {
        for (int step = 0; step < 80; ++step) {
            const int distance = std::abs(40 - step);
            hold(1300 + distance * 20, 800 + distance * 2, 1);
        }
    };
    flex(); flex(); flex();
    require(live.range()[1][0] > 2000 && !live.range()[1][4],
            "Continuous movement learns bend while a weak splay signal awaits further visits");
    flex(); flex(); flex();
    require(live.range()[1][4] == 0, "Unclenching cannot establish a cold-start splay range");
    hold(1300, 800, 80);
    for (int i = 0; i < 8; ++i) { hold(1300, 880, 40); hold(1300, 800, 40); }
    require(live.range()[0][4] == 800 && live.range()[1][4] == 880,
            "Independent small spread strokes can teach a weak splay sensor");
}

void checkLiveReceiver(const std::filesystem::path& path) {
    {
        auto transport = port();
        UdCapV1Core core(transport);
        connect(*transport, glove);
        core.configureHandCalibration(glove, true);
        Sample sample{};
        for (int cycle = 0; cycle < 3; ++cycle) {
            for (const int bend : {1000, 2000, 1000}) {
                sample[3] = static_cast<int16_t>(bend);
                for (int i = 0; i < 8; ++i) frame(*transport, sample);
            }
        }
        const auto range = core.liveHandRange();
        require(range && (*range)[0][3] == 1000 && (*range)[1][3] == 2000, "Receiver learns without guided poses");
        core.configureHandCalibration(glove, false);
        std::mutex mutex;
        std::condition_variable changed;
        bool angles = false, skeleton = false;
        const auto unlisten = core.listen([&](const auto& packet) {
            std::lock_guard lock(mutex);
            angles |= packet->commandType == CMD_ANGLE;
            skeleton |= packet->commandType == CMD_SKELETON_QUATERNION;
            changed.notify_all();
        });
        frame(*transport, sample);
        {
            std::unique_lock lock(mutex);
            const bool ready = changed.wait_for(lock, std::chrono::seconds(2), [&] { return angles && skeleton; });
            lock.unlock();
            unlisten();
            require(ready, "Frozen learned ranges still publish modeled angles and skeleton frames");
        }
        core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
        require(!core.liveHandRange(), "Pause learning during a guided capture");
        core.tryRestoreHandCalibration();
        require(core.liveHandRange() == range, "Cancel guided capture back to the live range");
        require(core.savePref(path.string()), "Save live endpoints without guided captures");
    }
    auto transport = port();
    UdCapV1Core core(transport);
    connect(*transport, glove);
    require(core.loadPref(path.string()), "Read a live-only profile");
    core.configureHandCalibration(glove, false);
    const auto restored = core.liveHandRange();
    require(restored && (*restored)[1][3] == 2000 && !core.handCalibrationSamples(),
            "Restart with learned endpoints and preserve the distinction from raw pose captures");
    send(*transport, CMD_LINK_STATE, {0});
    require(!core.liveHandRange(), "Disconnected gloves have no active range");
    connect(*transport, glove);
    require(core.liveHandRange() == restored, "A same-glove radio reconnect retains confirmed endpoints");
    const auto exercise = [&] {
        Sample sample{};
        for (int cycle = 0; cycle < 4; ++cycle)
            for (int bend : {1200, 2200, 1200}) {
                sample[2] = static_cast<int16_t>(bend);
                for (int i = 0; i < 8; ++i) frame(*transport, sample);
            }
    };
    exercise();
    require(core.liveHandRange() == restored, "Movement cannot teach new ranges while automatic calibration is off");
    core.configureHandCalibration(glove, true);
    exercise();
    require(core.liveHandRange().value()[1][2] == 2200, "Re-enabling learns new movement on the connected glove");
    UdCapV1Core::CalibrationSamples guided{};
    for (bool automatic : {true, false}) {
        core.configureHandCalibration(glove, automatic);
        guided[0].fill(automatic ? 1850 : 1975);
        guided[1].fill(800);
        guided[2].fill(1500);
        calibrate(core, *transport, guided);
        const auto expected = opencap::sensorRange(guided);
        require(core.liveHandRange() == expected, "Guided capture replaces all learned ranges in either mode");
        core.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
        core.tryRestoreHandCalibration();
        require(core.liveHandRange() == expected, "Canceling preserves the last completed guide");
        require(core.savePref(path.string()), "Save the replacement endpoints");
        auto freshPort = port();
        UdCapV1Core restarted(freshPort);
        connect(*freshPort, glove);
        require(restarted.loadPref(path.string()), "Load the guided profile");
        restarted.configureHandCalibration(glove, automatic);
        require(restarted.liveHandRange() == expected, "Restart uses the replacement endpoints");
    }
    connect(*transport, spare);
    require(!core.liveHandRange() && core.loadPref(path.string()), "Glove swap clears live state");
    core.configureHandCalibration(spare, true);
    require(core.liveHandRange() == opencap::SensorRange{}, "Another serial cannot inherit learned endpoints");
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
    core.mcuTriggerClick();
    require(writes.back() == std::vector<std::uint8_t>{85, 170, 1, 4, 4, 2, 0, 4, 5, 20},
            "Trigger click addresses a single legacy motor for 40 ms");
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
    core.mcuTriggerClick();
    require(writes.back() == std::vector<std::uint8_t>{85, 170, 1, 0xce, 8, 2, 0, 188, 2, 100, 0, 40, 0, 35},
            "Trigger click uses the short Controller 2.0 actuator command");
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

void checkRecording(const std::filesystem::path& folder) {
    using namespace opencap;
    CalibrationLog log;
    SensorFrame frame{{123, -45, 78}, 3, sensorTimeUs()};
    SensorRange range{};
    range[1][0] = 876;
    CalibrationRecord row{frame.timeUs, "frame", "left", glove, false, 4, frame, range};
    log.record(row);
    require(!std::filesystem::exists(folder), "Disabled recording creates no files");
    log.start(folder);
    log.record(row);
    row.event = "fist";
    row.timeUs += 200;
    log.record(row);
    log.stop();
    require(log.takeError().empty(), "Stop flushes queued samples and poses");
    const auto file = std::filesystem::directory_iterator(folder)->path();
    std::ifstream input(file);
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(input, line)) rows.push_back(nlohmann::json::parse(line));
    require(rows.size() == 4 && rows.front()["schema"] == 1 && rows.back()["event"] == "recording_stop" &&
            rows[1]["raw"] == std::vector<int>({123,-45,78}) && rows[1]["range"] == range &&
            rows[2]["event"] == "fist" && rows[2]["guide_id"] == 4 && rows[2]["serial"] == glove &&
            rows[2]["automatic"] == false && rows[2]["sample_utc_us"] == rows[1]["utc_us"] &&
            rows[2]["utc_us"] == row.timeUs, "Recorded pose references the original unpadded sensor frame");
    input.close();
    std::filesystem::remove(file);
    std::filesystem::remove(folder);
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
        opencap::TriggerClick click;
        require(!click.update(true, true, 1) && !click.update(true, false, 0) && click.update(true, true, 1) &&
                !click.update(true, true, 1) && !click.update(true, false, .99f) && !click.update(true, true, 1) &&
                !click.update(true, false, .5f) && click.update(true, true, 1) && !click.update(false, false, 0) &&
                !click.update(false, true, 1), "Pulse once per pull, with hysteresis and haptics enable gating");
        checkRecording(path.string() + ".logs");
        checkLiveRanges();
        checkLiveReceiver(path);
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
