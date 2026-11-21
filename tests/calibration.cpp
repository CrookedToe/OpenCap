#include <CorePref.h>
#include <UdCapV1Core.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
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

void frame(PortAccessor& port, const Sample& sample) {
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
    frame(port, {});
}

std::shared_ptr<PortAccessor> port() {
    auto result = std::make_shared<PortAccessor>();
    ManualSerialDriverCallbacks callbacks;
    callbacks.write = [](const std::uint8_t*, std::size_t count) { return count; };
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
            require(core.savePref(path.string()), "Save calibration using the normal glove profile");
        }
        {
            auto transport = port();
            UdCapV1Core core(transport);
            connect(*transport, glove);
            require(core.loadPref(path.string()) && core.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED &&
                    core.handCalibrationSamples() == captures, "Restore calibration in a new core instance");
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
