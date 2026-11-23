#include "receiver.hpp"
#include "bridge.hpp"

#include <CorePref.h>
#include <UdCapProbe.h>
#include <UdCapV1Core.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>

namespace opencap {
namespace {
using namespace std::chrono_literals;

UdTarget receiverSide(std::string_view serial) {
    if (serial.empty()) return UD_TARGET_UNKNOWN;
    const char side = serial.starts_with("UDXST1") && serial.size() > 6 ? serial[6] : serial.back();
    if (side == 'L') return UD_TARGET_LEFT_HAND;
    if (side == 'R') return UD_TARGET_RIGHT_HAND;
    return UD_TARGET_UNKNOWN;
}

std::optional<std::vector<std::wstring>> receiverPorts(bool wired = false) {
    auto comPort = GUID{0x86e0d1e0, 0x8089, 0x11d0, {0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73}};
    ULONG length{};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (CM_Get_Device_Interface_List_SizeW(&length, &comPort, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
            != CR_SUCCESS) return std::nullopt;
        std::vector<wchar_t> names(length);
        const auto result = CM_Get_Device_Interface_ListW(&comPort, nullptr, names.data(), length,
                                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_BUFFER_SMALL) continue;
        if (result != CR_SUCCESS) return std::nullopt;
        std::vector<std::wstring> ports;
        for (const auto* name = names.data(); *name; name += std::wcslen(name) + 1) {
            auto path = std::wstring(name);
            auto hardware = path;
            CharUpperBuffW(hardware.data(), static_cast<DWORD>(hardware.size()));
            constexpr std::array receiverIds{L"VID_1A86&PID_7523", L"VID_1A86&PID_7524", L"VID_1A85&PID_7524",
                                             L"VID_1A86&PID_0001", L"VID_1209&PID_0001"};
            if (!hardware.starts_with(L"\\\\?\\USB#")) continue;
            const auto usb = std::wstring_view(hardware).substr(8);
            if (wired ? usb.starts_with(L"VID_1915&PID_521F") :
                        std::ranges::any_of(receiverIds, [&](auto id) { return usb.starts_with(id); }))
                ports.push_back(std::move(path));
        }
        return ports;
    }
    return std::nullopt;
}

bool configureSerial(HANDLE port, int baud) {
    DCB settings{};
    settings.DCBlength = sizeof(settings);
    if (!GetCommState(port, &settings)) return false;
    settings.BaudRate = baud;
    settings.fBinary = TRUE;
    settings.fParity = settings.fOutxCtsFlow = settings.fOutxDsrFlow = FALSE;
    settings.fDsrSensitivity = settings.fInX = settings.fOutX = settings.fAbortOnError = FALSE;
    settings.fDtrControl = DTR_CONTROL_ENABLE;
    settings.fRtsControl = RTS_CONTROL_DISABLE;
    settings.ByteSize = 8;
    settings.Parity = NOPARITY;
    settings.StopBits = ONESTOPBIT;
    return SetCommState(port, &settings) != FALSE;
}

class WiredPort {
public:
    explicit WiredPort(const std::wstring& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open the wired glove port.");
        COMMTIMEOUTS timeouts{MAXDWORD, MAXDWORD, 50, 0, 1000};
        if (!configureSerial(handle_, 115200) || !SetCommTimeouts(handle_, &timeouts) ||
            !PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
            CloseHandle(handle_);
            throw std::runtime_error("Could not configure the wired glove port.");
        }
    }
    ~WiredPort() { CloseHandle(handle_); }
    WiredPort(const WiredPort&) = delete;
    WiredPort& operator=(const WiredPort&) = delete;

    void write(std::span<const std::uint8_t> bytes) {
        DWORD count{};
        if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
            throw std::runtime_error("The wired glove disconnected during a write.");
    }
    std::size_t read(std::span<std::uint8_t> target) {
        DWORD count{};
        if (!ReadFile(handle_, target.data(), static_cast<DWORD>(target.size()), &count, nullptr))
            throw std::runtime_error("The wired glove disconnected during a read.");
        return count;
    }

    char identify() {
        constexpr std::array<std::uint8_t, 4> query{0x44, 0x59, 0x04, 0xff};
        write(query);
        const auto until = std::chrono::steady_clock::now() + 5s;
        std::array<std::uint8_t, 128> bytes{};
        std::string response;
        while (std::chrono::steady_clock::now() < until && response.size() < 4096) {
            const auto count = read(bytes);
            response.append(reinterpret_cast<const char*>(bytes.data()), count);
            for (auto newline = response.find('\n'); newline != std::string::npos; newline = response.find('\n')) {
                if (const auto side = wiredGloveSide(std::string_view(response).substr(0, newline))) return side;
                response.erase(0, newline + 1);
            }
        }
        throw std::runtime_error("The wired glove did not report its serial and hand. Firmware was not sent.");
    }

private:
    HANDLE handle_;
};

class SerialPort {
public:
    explicit SerialPort(std::wstring path) : path_(std::move(path)) {
        ManualSerialDriverCallbacks callbacks;
        callbacks.open = [this] { return open(); };
        callbacks.close = [this] { close(); };
        callbacks.setBaudRate = [this](int baud) {
            std::lock_guard lock(writes_);
            if (handle_ == INVALID_HANDLE_VALUE || !configureSerial(handle_, baud))
                throw std::runtime_error("Could not configure the UDCAP serial port.");
        };
        callbacks.write = [this](const std::uint8_t* data, std::size_t length) {
            std::lock_guard lock(writes_);
            DWORD written{};
            if (handle_ == INVALID_HANDLE_VALUE ||
                !WriteFile(handle_, data, static_cast<DWORD>(length), &written, nullptr)) return std::size_t{};
            return static_cast<std::size_t>(written);
        };
        callbacks.waitForPendingWrites = [this](std::chrono::milliseconds timeout) {
            const auto until = std::chrono::steady_clock::now() + timeout;
            std::lock_guard lock(writes_);
            do {
                COMSTAT state{};
                DWORD errors{};
                if (handle_ == INVALID_HANDLE_VALUE || !ClearCommError(handle_, &errors, &state)) return false;
                if (!state.cbOutQue) return true;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < until);
            return false;
        };
        accessor_->bindSerialCallbacks(std::move(callbacks));
    }

    ~SerialPort() {
        accessor_->closePort();
        close();
    }
    const std::shared_ptr<PortAccessor>& accessor() const { return accessor_; }
    const std::wstring& path() const { return path_; }

private:
    bool open() {
        close();
        handle_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        COMMTIMEOUTS timeouts{MAXDWORD, MAXDWORD, 30, 0, 500};
        if (!SetCommTimeouts(handle_, &timeouts) || !configureSerial(handle_, 115200)) {
            close();
            return false;
        }
        reader_ = std::jthread([this](std::stop_token stop) {
            std::array<std::uint8_t, 256> bytes{};
            while (!stop.stop_requested()) {
                DWORD count{};
                if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
                    if (!stop.stop_requested())
                        accessor_->notifyConnectionState(PORT_ACCESSOR_CONNECTION_STATE_DISCONNECTED);
                    return;
                }
                if (count) accessor_->injectReceivedData(bytes.data(), count);
            }
        });
        return true;
    }

    void close() {
        if (reader_.joinable()) {
            reader_.request_stop();
            reader_.join();
        }
        std::lock_guard lock(writes_);
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    std::wstring path_;
    std::shared_ptr<PortAccessor> accessor_ = std::make_shared<PortAccessor>();
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::mutex writes_;
    std::jthread reader_;
};

template <std::size_t size>
void copyText(std::array<char, size>& dest, std::string_view value) {
    dest.fill(0);
    std::copy_n(value.data(), (std::min)(value.size(), size - 1), dest.data());
}

float unit(float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; }
float axis(float value) { return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f; }

std::array<BoneRotation*, jointCount> jointRotations(HandRotation& hand) {
    const std::array<FingerRotation*, 5> fingers{&hand.thumbFinger, &hand.indexFinger, &hand.middleFinger,
                                               &hand.ringFinger, &hand.littleFinger};
    std::array<BoneRotation*, jointCount> joints{};
    for (std::size_t i = 0; i < fingers.size(); ++i) {
        joints[3*i] = &fingers[i]->proximal;
        joints[3*i + 1] = &fingers[i]->intermediate;
        joints[3*i + 2] = &fingers[i]->distal;
    }
    return joints;
}

class Device {
public:
    explicit Device(std::unique_ptr<SerialPort> port, UdTarget fallback)
        : port_(std::move(port)), core_(port_->accessor()) {
        sample_.receiver = true;
        core_.setFallbackTarget(fallback);
        unlisten_ = core_.listen([this](const auto& packet) { receive(*packet); });
        core_.mcuGetLinkState();
        core_.mcuGetFirmwareVersion();
        core_.mcuGetChannel();
        readButtonRanges();
    }

    ~Device() { if (unlisten_) unlisten_(); }

    UdTarget side() const { return core_.getTarget(); }
    const std::wstring& portPath() const { return port_->path(); }
    std::string gloveSerial() const { return core_.getUDCapSerial(); }
    bool portOpen() const { return port_->accessor()->isOpen(); }

    Hand read() const {
        const auto revision = core_.getControllerRevision();
        std::lock_guard lock(mutex_);
        auto result = sample_;
        result.controllerRevision = revision;
        return result;
    }

    std::optional<CalibrationSamples> calibration() const { return core_.handCalibrationSamples(); }
    HandCurves handCurves() const {
        const auto current = core_.getHandResponse();
        HandCurves curves;
        std::ranges::transform(current, curves.begin(), [](const auto& curve) {
            return HandCurve{curve.outputMin, curve.outputMax, curve.balance, curve.offset};
        });
        return curves;
    }

    void expireChannelRequest() {
        const auto time = now();
        std::lock_guard lock(mutex_);
        if (channelRequested_ && time - channelRequested_ >= 10000) {
            channelRequested_ = 0;
            sample_.pendingChannel = -1;
            copyText(sample_.message, "Receiver did not confirm the channel. Check the channel before retrying.");
        }
    }

    void restorePreferences(const std::filesystem::path& directory, const ReceiverPreferences& defaults,
                            std::uint64_t revision) {
        if (core_.getState() != UD_INIT_STATE_LINKED) return;
        const auto serial = core_.getUDCapSerial();
        if (serial.empty()) return;
        if (serial != loadedSerial_) {
            loadedSerial_ = serial;
            loadedRevision_ = 0;
            try { core_.loadPref(); readButtonRanges(); readSplayCurves(); }
            catch (const std::exception& error) { report(error.what()); }
        }
        if (read().calibrationStep != 0) return;
        if (core_.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_NONE && core_.handCalibrationSamples()) {
            try { core_.tryRestoreHandCalibration(); }
            catch (const std::exception& error) { report(error.what()); }
        }
        if (loadedRevision_ == revision) return;
        loadedRevision_ = revision;
        if (defaults.empty() || !revision) return;
        try {
            const auto marker = directory / (serial + ".udcap-revision");
            std::uint64_t previouslyImported{};
            if (std::ifstream{marker} >> previouslyImported && previouslyImported == revision) return;
            import(directory, defaults, serial);
            readButtonRanges();
            readSplayCurves();
            std::ofstream output(marker, std::ios::trunc);
            output << revision << '\n';
            output.close();
            if (!output) throw std::runtime_error("Could not record the glove settings import.");
        } catch (const std::exception& error) { report(error.what()); }
    }

    void command(const Command& request, const std::filesystem::path& directory) {
        const auto previous = read();
        auto stage = previous.calibrationStep;
        auto calibrationResult = previous.calibrationResult;
        const bool handCalibration = isHandCalibration(request.action);
        if (handCalibration && request.action != Action::startHand && previous.calibrationId != request.value) return;
        if (request.action == Action::startHand && stage >= 1 && stage <= 3) return;
        try {
            if (request.action != Action::pairStart && request.action != Action::pairStop &&
                request.action != Action::pairBoth && request.action != Action::pairBothStop &&
                request.action != Action::channel && core_.getState() != UD_INIT_STATE_LINKED)
                throw std::runtime_error("Connect the glove first.");
            switch (request.action) {
            case Action::startHand:
                if (stage == 4) throw std::runtime_error("Finish the joystick calibration first.");
                core_.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                stage = 1;
                calibrationResult = CalibrationResult::active;
                break;
            case Action::fist:
                if (stage == 1) {
                    capture(UDCAP_V1_HAND_CALI_TYPE_FIST);
                    stage = 2;
                }
                break;
            case Action::together:
                if (stage == 2) {
                    capture(UDCAP_V1_HAND_CALI_TYPE_ADDUCTION);
                    stage = 3;
                }
                break;
            case Action::spread:
                if (stage == 3) {
                    capture(UDCAP_V1_HAND_CALI_TYPE_PROTRACT);
                    stage = 0;
                    core_.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                    save(directory);
                    calibrationResult = CalibrationResult::saved;
                }
                break;
            case Action::cancelHand:
                if (stage >= 1 && stage <= 3) {
                    core_.tryRestoreHandCalibration();
                    stage = 0;
                    calibrationResult = CalibrationResult::canceled;
                }
                break;
            case Action::joystickCenter:
                if (stage >= 1 && stage <= 3) throw std::runtime_error("Finish or cancel hand calibration first.");
                core_.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_JOYSTICK);
                core_.captureJoystickData(UDCAP_V1_JOYSTICK_CALI_TYPE_CENTER);
                stage = 4;
                break;
            case Action::joystickRange:
                if (stage == 4) core_.captureJoystickData(UDCAP_V1_JOYSTICK_CALI_TYPE_ZONE);
                break;
            case Action::joystickFinish:
                if (stage == 4) {
                    core_.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_JOYSTICK);
                    stage = 0;
                    save(directory);
                }
                break;
            case Action::pairStart: core_.mcuStartPairing(); break;
            case Action::pairStop: core_.mcuStopPairing(); break;
            case Action::pairBoth: core_.mcuStartPairingBoth(); break;
            case Action::pairBothStop: core_.mcuStopPairingBoth(); break;
            case Action::channel:
                if (request.value < 0 || request.value > 100) throw std::runtime_error("Channel must be 0 to 100.");
                changeChannel(static_cast<std::uint8_t>(request.value));
                return;
            case Action::haptic:
                core_.mcuSendHaptic(request.duration, request.amplitude, request.extra);
                break;
            case Action::hapticTest:
                core_.mcuTestHaptic(request.value == 1);
                break;
            case Action::setHandCurve: {
                const auto channel = static_cast<std::size_t>(request.value);
                if (!validHandCurve(channel, request.curve))
                    throw std::runtime_error("Invalid hand response curve.");
                const auto previous = core_.getHandResponse();
                auto updated = previous;
                const auto& curve = request.curve;
                updated[channel] = {curve.min, curve.max, curve.balance, curve.offset};
                core_.setHandResponse(updated);
                try { save(directory); }
                catch (...) { core_.setHandResponse(previous); throw; }
                readSplayCurves();
                break;
            }
            case Action::setJointOffset: {
                const auto index = static_cast<std::size_t>(request.value);
                const Vec3 offset{request.duration, request.amplitude, request.extra};
                if (index >= jointCount || !validJointOffset(offset)) throw std::runtime_error("Invalid joint adjustment.");
                auto angles = core_.getHandOffset();
                *jointRotations(angles)[index] = {offset.x, offset.y, offset.z};
                core_.setHandOffset(angles);
                save(directory);
                break;
            }
            case Action::setButtonRanges: {
                if (!std::ranges::all_of(request.ranges, validButtonRange))
                    throw std::runtime_error("Invalid controller thresholds.");
                core_.setTriggerButtonMin(request.ranges[0].touch);
                core_.setTriggerButtonMax(request.ranges[0].click);
                core_.setGripButtonMin(request.ranges[1].touch);
                core_.setGripButtonMax(request.ranges[1].click);
                core_.setTrackpadButtonMin(request.ranges[2].touch);
                core_.setTrackpadButtonMax(request.ranges[2].click);
                readButtonRanges();
                save(directory);
                break;
            }
            case Action::toggleStick:
            case Action::adjustPose:
            case Action::applyPreset:
            case Action::guidedCalibration:
            case Action::togglePublishing:
            case Action::toggleHaptics:
            case Action::cycleTracker:
            case Action::autoTracker: break;
            }
            std::lock_guard lock(mutex_);
            sample_.calibrationStep = stage;
            if (handCalibration) {
                sample_.calibrationId = request.value;
                sample_.calibrationResult = calibrationResult;
            }
            if (request.action == Action::hapticTest) copyText(sample_.message, "Vibration test sent to the glove.");
            else if (request.action != Action::haptic) copyText(sample_.message, {});
        } catch (const std::exception& error) {
            if (handCalibration && stage >= 1 && stage <= 3) {
                try { core_.tryRestoreHandCalibration(); } catch (const std::exception&) {}
                stage = 0;
            }
            std::lock_guard lock(mutex_);
            sample_.calibrationStep = stage;
            if (handCalibration) {
                sample_.calibrationId = request.value;
                sample_.calibrationResult = CalibrationResult::failed;
            }
            copyText(sample_.message, error.what());
        }
    }

private:
    void changeChannel(std::uint8_t channel) {
        {
            std::lock_guard lock(mutex_);
            if (channelRequested_) throw std::runtime_error("Wait for the current channel change.");
            if (channel == sample_.channel) return;
            channelRequested_ = now();
            sample_.pendingChannel = channel;
            copyText(sample_.message, "Waiting for receiver to confirm channel change.");
        }
        try { core_.mcuSetChannel(channel); }
        catch (...) {
            std::lock_guard lock(mutex_);
            channelRequested_ = 0;
            sample_.pendingChannel = -1;
            throw;
        }
    }

    void capture(UdCapV1HandCaliType pose) {
        const auto time = now();
        const auto last = read().lastFrame;
        if (!last || time < last || time - last > sampleTimeout)
            throw std::runtime_error("Wait for live glove data before capturing the pose.");
        core_.captureCalibrationData(pose);
    }

    void readButtonRanges() {
        const ButtonRanges ranges{{{core_.getTriggerButtonMin(), core_.getTriggerButtonMax()},
            {core_.getGripButtonMin(), core_.getGripButtonMax()}, {core_.getTrackpadButtonMin(), core_.getTrackpadButtonMax()}}};
        std::lock_guard lock(mutex_);
        sample_.buttonRanges = ranges;
    }

    void readSplayCurves() {
        const auto curves = handCurves();
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < sample_.splayCurves.size(); ++i)
            sample_.splayCurves[i] = curves[splayCurveChannels[i]];
    }

    void report(std::string_view message) {
        std::lock_guard lock(mutex_);
        copyText(sample_.message, message);
    }

    static std::filesystem::path profilePath(const std::filesystem::path& directory, const std::string& serial) {
        if (serial.size() > 32 || serial.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
            throw std::runtime_error("Invalid glove identity.");
        if (serial.empty()) throw std::runtime_error("Wait for the glove to report its serial number.");
        std::filesystem::create_directories(directory);
        return directory / (serial + ".json");
    }

    struct RemoveTemporary {
        const std::filesystem::path& path;
        ~RemoveTemporary() { std::error_code error; std::filesystem::remove(path, error); }
    };

    void import(const std::filesystem::path& directory, const ReceiverPreferences& defaults, const std::string& serial) {
        const auto target = profilePath(directory, serial);
        const auto temporary = directory / (serial + ".udcap-import.tmp");
        const RemoveTemporary cleanup{temporary};
        if (!core_.savePref(temporary.string())) throw std::runtime_error("Could not prepare glove settings.");

        nlohmann::json next;
        {
            std::ifstream input(temporary);
            input >> next;
        }
        for (const auto& [key, value] : defaults.controller) next["controller"][key] = value;
        if (defaults.hasCurves) {
            auto curves = nlohmann::json::array();
            for (const auto& curve : defaults.curves)
                curves.push_back({{"min", curve.min}, {"max", curve.max}, {"balance", curve.balance}, {"offset", curve.offset}});
            next["handResponse"] = std::move(curves);
        }
        {
            std::ofstream output(temporary, std::ios::trunc);
            output << next.dump(2) << '\n';
            output.close();
            if (!output) throw std::runtime_error("Could not write imported glove settings.");
        }
        if (!core_.loadPref(temporary.string())) throw std::runtime_error("The glove rejected the imported preferences.");
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Glove settings were applied but could not be saved.");
    }

    void save(const std::filesystem::path& directory) {
        const auto serial = core_.getUDCapSerial();
        const auto target = profilePath(directory, serial);
        const auto temporary = directory / (serial + ".opencap-save.tmp");
        const RemoveTemporary cleanup{temporary};
        if (!core_.savePref(temporary.string()) || core_.getUDCapSerial() != serial ||
            !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not save glove settings.");
    }

    void receive(const UdCapV1MCUPacket& packet) {
        const auto battery = packet.commandType == CMD_BATTERY ? core_.getBattery() : std::nullopt;
        const auto signal = packet.commandType == CMD_BATTERY ? core_.getRssiDbm() : std::nullopt;
        const bool calibrated = core_.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED;
        const auto captures = packet.commandType == CMD_READY ? core_.handCalibrationSamples() : std::nullopt;
        std::lock_guard lock(mutex_);
        sample_.calibrated = calibrated;
        switch (packet.commandType) {
        case CMD_PORT_CONNECTION_STATE: {
            const auto ranges = sample_.buttonRanges;
            const auto curves = sample_.splayCurves;
            sample_ = {};
            sample_.receiver = packet.portConnectionState == PORT_ACCESSOR_CONNECTION_STATE_CONNECTED;
            sample_.buttonRanges = ranges;
            sample_.splayCurves = curves;
            channelRequested_ = 0;
            splayCalibration_.reset();
            break;
        }
        case CMD_LINK_STATE:
            sample_.linked = packet.udState == UD_INIT_STATE_LINKED;
            if (!sample_.linked) {
                sample_.lastFrame = 0;
                sample_.calibrationStep = 0;
                sample_.calibrationId = 0;
                sample_.calibrationResult = CalibrationResult::idle;
                sample_.quality = {};
                sample_.splay = {};
                splayCalibration_.reset();
                sample_.buttons = 0;
                sample_.battery = -1;
                sample_.signal = 0;
                sample_.fps = 0;
                fpsStart_ = 0;
                frames_ = 0;
            }
            break;
        case CMD_DATA: {
            const auto time = now();
            sample_.linked = true;
            if (calibrated && splayCalibration_) {
                const auto next = calibratedSplay(*splayCalibration_, packet.angle);
                if (!sample_.splay.measured || !sample_.lastFrame) sample_.splay = next;
                else {
                    const auto weight = std::clamp(static_cast<float>(time - sample_.lastFrame) / 22.0f, 0.0f, 1.0f);
                    for (std::size_t gap = 0; gap < next.gaps.size(); ++gap)
                        sample_.splay.gaps[gap] = std::lerp(sample_.splay.gaps[gap], next.gaps[gap], weight);
                }
            } else sample_.splay = {};
            sample_.lastFrame = time;
            if (fpsStart_ == 0) fpsStart_ = time;
            ++frames_;
            if (time - fpsStart_ >= 1000) {
                sample_.fps = 1000.0f * frames_ / static_cast<float>(time - fpsStart_);
                frames_ = 0;
                fpsStart_ = time;
            }
            break;
        }
        case CMD_SERIAL:
            sample_.lastFrame = 0;
            sample_.calibrationStep = 0;
            sample_.calibrationId = 0;
            sample_.calibrationResult = CalibrationResult::idle;
            sample_.quality = {};
            sample_.splay = {};
            splayCalibration_.reset();
            break;
        case CMD_READY:
            splayCalibration_ = captures;
            sample_.splay = {};
            sample_.quality = captures ? calibrationSpans(*captures) : CalibrationSpans{};
            break;
        case CMD_FW_VERSION: copyText(sample_.firmware, packet.fwVersion); break;
        case CMD_GET_CHANNEL:
            if (packet.channel > 100) break;
            if (sample_.channel != packet.channel) sample_.signal = 0;
            sample_.channel = packet.channel;
            if (packet.channel == sample_.pendingChannel) {
                channelRequested_ = 0;
                sample_.pendingChannel = -1;
                copyText(sample_.message, "Receiver confirmed the channel change.");
            }
            break;
        case CMD_SET_CHANNEL:
            if (packet.channel == sample_.pendingChannel && packet.channelResult != 2) {
                channelRequested_ = 0;
                sample_.pendingChannel = -1;
                copyText(sample_.message, "Receiver could not change the channel.");
            }
            break;
        case CMD_SET_CHANNEL_DONE: break;
        case CMD_PAIRING:
        case CMD_PAIRING_BOTH: sample_.pairing = packet.pairing; break;
        case CMD_BATTERY:
            if (battery) sample_.battery = unit(battery->first / 5.0f);
            if (signal && sample_.pendingChannel < 0) sample_.signal = -std::abs(*signal);
            break;
        case CMD_INPUT_JOYSTICK:
            sample_.stickX = axis(packet.joystickData.joyX);
            sample_.stickY = axis(packet.joystickData.joyY);
            break;
        case CMD_ANGLE:
            std::transform(packet.result.begin(), packet.result.begin() + frameAngles_.size(), frameAngles_.begin(),
                             [](double value) { return static_cast<float>(value); });
            {
                auto offsets = packet.angleBoneOffset;
                const auto joints = jointRotations(offsets);
                for (std::size_t i = 0; i < joints.size(); ++i)
                    frameJointOffsets_[i] = {joints[i]->x, joints[i]->y, joints[i]->z};
            }
            break;
        case CMD_INPUT_BUTTON: {
            const auto& b = packet.button;
            if (b.btnPower && !(sample_.buttons & bit(Button::power))) ++sample_.calibrationRequests;
            sample_.buttons = (b.btnA ? bit(Button::a) : 0) | (b.btnB ? bit(Button::b) : 0)
                | (b.btnMenu ? bit(Button::system) : 0) | (b.btnJoyStick ? bit(Button::stick) : 0)
                | (b.btnPower ? bit(Button::power) : 0) | (b.btnTrigger ? bit(Button::trigger) : 0)
                | (b.btnGrip ? bit(Button::grip) : 0) | (b.btnTrackpad ? bit(Button::pad) : 0);
            sample_.trigger = unit(b.trigger);
            sample_.grip = unit(b.grip);
            sample_.pad = unit(b.trackpad);
            break;
        }
        case CMD_SKELETON_QUATERNION: {
            sample_.angles = frameAngles_;
            sample_.jointOffsets = frameJointOffsets_;
            const auto& q = packet.skeletonQuaternion;
            const std::array fingers{q.thumbFinger, q.indexFinger, q.middleFinger, q.ringFinger, q.littleFinger};
            for (std::size_t finger = 0; finger < fingers.size(); ++finger) {
                const auto& f = fingers[finger];
                const std::array joints{f.proximal, f.intermediate, f.distal};
                for (std::size_t joint = 0; joint < joints.size(); ++joint) {
                    const auto& value = joints[joint];
                    sample_.joints[finger * 3 + joint].rotation = normalize({value.x, value.y, value.z, value.w});
                }
                const auto proximal = sample_.joints[finger * 3].rotation;
                const auto bend = finger == 0 ? proximal.y : proximal.z;
                sample_.curl[finger] = unit(2.0f * std::atan2(std::abs(bend), std::abs(proximal.w)) / 1.35f);
            }
            break;
        }
        default: break;
        }
    }

    mutable std::mutex mutex_;
    Hand sample_;
    std::array<float, 23> frameAngles_{};
    std::array<Vec3, jointCount> frameJointOffsets_{};
    Millis fpsStart_{};
    Millis channelRequested_{};
    int frames_{};
    std::string loadedSerial_;
    std::optional<CalibrationSamples> splayCalibration_;
    std::uint64_t loadedRevision_{};
    std::unique_ptr<SerialPort> port_;
    UdCapV1Core core_;
    std::function<void()> unlisten_;
};

std::shared_ptr<Device> selectDevice(const std::vector<std::shared_ptr<Device>>& devices, UdTarget side) {
    std::shared_ptr<Device> selected;
    Millis latest = 0;
    for (const auto& device : devices) {
        if (device->side() != side) continue;
        const auto frame = device->read().lastFrame;
        if (!selected || frame > latest) {
            selected = device;
            latest = frame;
        }
    }
    return selected;
}
}

void updateWiredDevice(const FirmwareUpdateImage& image, const FirmwareProgress& progress) {
    const bool receiver = image.target == FirmwareTarget::receiver;
    const auto ports = receiverPorts(!receiver);
    if (!ports || ports->size() != 1) {
        if (receiver) throw std::runtime_error("Connect just the UDCAP receiver you want to update, then try again.");
        throw std::runtime_error("Connect one UDCAP glove using its USB data port, then try again.");
    }
    if (receiver) {
        SerialPort serial(ports->front());
        UdCapProbe probe(serial.accessor());
        if (probe.probe() != UDCAP_PROBE_HAND_V1)
            throw std::runtime_error("Could not confirm a single-hand receiver. This image cannot be used on a combined receiver.");
    }
    WiredPort port(ports->front());
    if (!receiver) {
        const auto side = port.identify();
        if (image.target != (side == 'L' ? FirmwareTarget::leftGlove : FirmwareTarget::rightGlove))
            throw std::runtime_error("The firmware image is for the other hand. No firmware was sent.");
    }
    sendFirmware(image.contents, [&](auto bytes) { port.write(bytes); },
                 [&](auto bytes) { return port.read(bytes); }, progress);
    // Leave USB connected while the updated device reboots before discovery resumes.
    std::this_thread::sleep_for(10s);
}

void updateWirelessGlove(const FirmwareUpdateImage& image, const WirelessFirmwareTarget& target,
                         const FirmwareProgress& progress) {
    const bool left = target.side == Side::left;
    if (image.target != (left ? FirmwareTarget::leftGlove : FirmwareTarget::rightGlove))
        throw std::runtime_error("Select a firmware image for the connected hand.");
    {
        auto serial = std::make_unique<SerialPort>(target.port);
        UdCapProbe probe(serial->accessor());
        if (probe.probe() == UDCAP_PROBE_FAILURE)
            throw std::runtime_error("The wireless receiver is no longer available.");
        Device device(std::move(serial), receiverSide(probe.getUDCapSerial()));
        const auto expectedSide = left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (device.gloveSerial() == target.serial && device.side() == expectedSide && device.read().lastFrame) break;
            std::this_thread::sleep_for(50ms);
        }
        if (device.gloveSerial() != target.serial || device.side() != expectedSide || !device.read().lastFrame)
            throw std::runtime_error("The receiver did not reconnect to the selected glove. Firmware was not sent.");
    }
    WiredPort port(target.port);
    constexpr std::array<std::uint8_t, 7> enterUpdate{0x55, 0xaa, 1, 0xa0, 1, 1, 0x56};
    port.write(enterUpdate);
    std::this_thread::sleep_for(200ms);
    sendFirmware(image.contents, [&](auto bytes) { port.write(bytes); },
                 [&](auto bytes) { return port.read(bytes); }, progress, FirmwareTransport::wireless);
}

struct Receiver::Impl {
    struct Defaults {
        std::array<ReceiverPreferences, handCount> hands;
        std::uint64_t revision{};
    };

    Impl(const std::filesystem::path& preferences, std::array<ReceiverPreferences, handCount> hands,
         std::uint64_t revision)
        : preferences(preferences), defaults(std::make_shared<Defaults>(Defaults{std::move(hands), revision})) {
        try { CorePref::getInstance().setDefaultPrefPath(preferences.string()); }
        catch (const std::exception&) {}
        worker = std::jthread([this](std::stop_token stop) { loop(stop); });
    }

    std::vector<std::shared_ptr<Device>> members() const {
        std::lock_guard lock(deviceMutex);
        std::vector<std::shared_ptr<Device>> result;
        for (const auto& entry : devices) result.push_back(entry.second);
        return result;
    }

    void refresh() {
        const auto ports = receiverPorts();
        if (!ports) return;
        std::vector<std::shared_ptr<Device>> removed;
        {
            std::lock_guard lock(deviceMutex);
            for (auto it = devices.begin(); it != devices.end();) {
                if (!it->second->portOpen() || std::ranges::find(*ports, it->first) == ports->end()) {
                    removed.push_back(std::move(it->second));
                    it = devices.erase(it);
                } else {
                    ++it;
                }
            }
        }
        removed.clear();
        for (const auto& port : *ports) {
            {
                std::lock_guard lock(deviceMutex);
                if (devices.contains(port)) continue;
            }
            try {
                auto serial = std::make_unique<SerialPort>(port);
                UdCapProbe probe(serial->accessor());
                if (probe.probe() == UDCAP_PROBE_FAILURE) continue;
                auto device = std::make_shared<Device>(std::move(serial), receiverSide(probe.getUDCapSerial()));
                std::lock_guard lock(deviceMutex);
                devices.emplace(port, std::move(device));
            } catch (const std::exception&) {
                // A busy or disconnected port will be tried during the next refresh.
            }
        }
    }

    void loop(std::stop_token stop) {
        auto nextRefresh = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            if (std::chrono::steady_clock::now() >= nextRefresh) {
                try { refresh(); } catch (const std::exception&) {}
                nextRefresh = std::chrono::steady_clock::now() + 3s;
            }
            std::deque<Command> pending;
            std::shared_ptr<const Defaults> currentDefaults;
            {
                std::unique_lock lock(commandMutex);
                changed.wait_for(lock, stop, 30ms, [&] { return !commands.empty(); });
                pending.swap(commands);
                currentDefaults = defaults;
            }
            const auto devicesNow = members();
            for (const auto& device : devicesNow) {
                device->expireChannelRequest();
                const auto side = device->side();
                if (side == UD_TARGET_UNKNOWN) continue;
                const auto index = side == UD_TARGET_LEFT_HAND ? 0u : 1u;
                device->restorePreferences(preferences, currentDefaults->hands[index],
                                           currentDefaults->revision);
            }
            for (const auto& command : pending) {
                if (stop.stop_requested()) break;
                const auto side = command.side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
                if (const auto device = selectDevice(devicesNow, side)) device->command(command, preferences);
            }
        }
    }

    mutable std::mutex deviceMutex;
    std::map<std::wstring, std::shared_ptr<Device>> devices;
    std::mutex commandMutex;
    std::condition_variable_any changed;
    std::deque<Command> commands;
    std::filesystem::path preferences;
    std::shared_ptr<const Defaults> defaults;
    std::jthread worker;
};

Receiver::Receiver(const std::filesystem::path& preferences, std::array<ReceiverPreferences, handCount> defaults,
                   std::uint64_t importRevision)
    : impl_(std::make_unique<Impl>(preferences, std::move(defaults), importRevision)) {}
Receiver::~Receiver() = default;

std::optional<CalibrationSamples> Receiver::calibration(Side side) const {
    const auto target = side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
    const auto selected = selectDevice(impl_->members(), target);
    return selected ? selected->calibration() : std::nullopt;
}

std::optional<HandCurves> Receiver::handCurves(Side side) const {
    const auto target = side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
    const auto selected = selectDevice(impl_->members(), target);
    return selected ? std::optional{selected->handCurves()} : std::nullopt;
}

std::optional<WirelessFirmwareTarget> Receiver::wirelessFirmwareTarget(Side side) const {
    const auto target = side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
    const auto selected = selectDevice(impl_->members(), target);
    if (!selected) return std::nullopt;
    const auto last = selected->read().lastFrame;
    auto serial = selected->gloveSerial();
    if (serial.empty() || !last || now() - last > sampleTimeout) return std::nullopt;
    return WirelessFirmwareTarget{selected->portPath(), std::move(serial), side};
}

std::array<Hand, handCount> Receiver::read() const {
    std::array<Hand, handCount> hands{};
    for (const auto& device : impl_->members()) {
        const auto side = device->side();
        if (side == UD_TARGET_UNKNOWN) continue;
        const auto index = side == UD_TARGET_LEFT_HAND ? 0u : 1u;
        const auto current = device->read();
        if (!hands[index].receiver || current.lastFrame > hands[index].lastFrame) hands[index] = current;
    }
    return hands;
}

bool Receiver::submit(const Command& command) {
    {
        std::lock_guard lock(impl_->commandMutex);
        if (impl_->commands.size() >= 32) return false;
        impl_->commands.push_back(command);
    }
    impl_->changed.notify_one();
    return true;
}

void Receiver::importPreferences(std::array<ReceiverPreferences, handCount> defaults, std::uint64_t importRevision) {
    std::lock_guard lock(impl_->commandMutex);
    impl_->defaults = std::make_shared<Impl::Defaults>(Impl::Defaults{std::move(defaults), importRevision});
}
}
