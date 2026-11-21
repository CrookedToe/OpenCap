#include "receiver.hpp"
#include "bridge.hpp"

#include <CorePref.h>
#include <UdCapProbe.h>
#include <UdCapV1Core.h>
#include <UsbEnumerate.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
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

bool isReceiver(std::uint16_t vendor, std::uint16_t product) {
    return vendor == 0x1a86 && (product == 0x7523 || product == 0x7524);
}

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
    explicit Device(std::shared_ptr<PortAccessor> port, UdTarget fallback)
        : core_(std::move(port)) {
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

    Hand read() const {
        std::lock_guard lock(mutex_);
        return sample_;
    }

    void restorePreferences(const std::filesystem::path& directory, const ReceiverPreferences& defaults,
                            std::uint64_t revision) {
        if (core_.getState() != UD_INIT_STATE_LINKED) return;
        const auto serial = core_.getUDCapSerial();
        if (serial.empty()) return;
        if (serial != loadedSerial_) {
            loadedSerial_ = serial;
            loadedRevision_ = 0;
            try { core_.loadPref(); readButtonRanges(); }
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
            std::ofstream output(marker, std::ios::trunc);
            output << revision << '\n';
            output.close();
            if (!output) throw std::runtime_error("Could not record the glove settings import.");
        } catch (const std::exception& error) { report(error.what()); }
    }

    void command(const Command& request, const std::filesystem::path& directory) {
        try {
            auto stage = read().calibrationStep;
            if (request.action != Action::pairStart && request.action != Action::pairStop &&
                request.action != Action::pairBoth && request.action != Action::pairBothStop &&
                request.action != Action::channel && core_.getState() != UD_INIT_STATE_LINKED)
                throw std::runtime_error("Connect the glove first.");
            switch (request.action) {
            case Action::startHand:
                if (stage == 4) throw std::runtime_error("Finish the joystick calibration first.");
                core_.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                stage = 1;
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
                    setCalibrationStep(0);
                    core_.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                    stage = 0;
                    save(directory);
                }
                break;
            case Action::cancelHand:
                if (stage >= 1 && stage <= 3) {
                    core_.tryRestoreHandCalibration();
                    stage = 0;
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
                    setCalibrationStep(stage);
                    save(directory);
                }
                break;
            case Action::pairStart: core_.mcuStartPairing(); break;
            case Action::pairStop: core_.mcuStopPairing(); break;
            case Action::pairBoth: core_.mcuStartPairingBoth(); break;
            case Action::pairBothStop: core_.mcuStopPairingBoth(); break;
            case Action::channel:
                if (request.value < 0 || request.value > 100) throw std::runtime_error("Channel must be 0 to 100.");
                core_.mcuSetChannel(static_cast<uint8_t>(request.value));
                core_.mcuGetChannel();
                break;
            case Action::haptic:
                core_.mcuSendHaptic(request.duration, request.amplitude, request.extra);
                break;
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
            case Action::guidedCalibration: break;
            }
            std::lock_guard lock(mutex_);
            sample_.calibrationStep = stage;
            if (request.action != Action::haptic) copyText(sample_.message, {});
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            copyText(sample_.message, error.what());
        }
    }

private:
    void setCalibrationStep(unsigned step) {
        std::lock_guard lock(mutex_);
        sample_.calibrationStep = step;
    }

    void capture(UdCapV1HandCaliType pose) {
        const auto time = now();
        const auto last = read().lastFrame;
        if (!last || (time >= last && time - last > sampleTimeout))
            throw std::runtime_error("Wait for live glove data before capturing the pose.");
        core_.captureCalibrationData(pose);
    }

    void readButtonRanges() {
        const ButtonRanges ranges{{{core_.getTriggerButtonMin(), core_.getTriggerButtonMax()},
            {core_.getGripButtonMin(), core_.getGripButtonMax()}, {core_.getTrackpadButtonMin(), core_.getTrackpadButtonMax()}}};
        std::lock_guard lock(mutex_);
        sample_.buttonRanges = ranges;
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
            sample_ = {};
            sample_.receiver = packet.portConnectionState == PORT_ACCESSOR_CONNECTION_STATE_CONNECTED;
            sample_.buttonRanges = ranges;
            break;
        }
        case CMD_LINK_STATE:
            sample_.linked = packet.udState == UD_INIT_STATE_LINKED;
            if (!sample_.linked) {
                sample_.lastFrame = 0;
                sample_.calibrationStep = 0;
                sample_.quality = {};
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
            sample_.quality = {};
            break;
        case CMD_READY:
            sample_.quality = captures ? calibrationSpans(*captures) : CalibrationSpans{};
            break;
        case CMD_FW_VERSION: copyText(sample_.firmware, packet.fwVersion); break;
        case CMD_GET_CHANNEL: sample_.channel = packet.channel; break;
        case CMD_SET_CHANNEL_DONE: break;
        case CMD_PAIRING:
        case CMD_PAIRING_BOTH: sample_.pairing = packet.pairing; break;
        case CMD_BATTERY:
            if (battery) sample_.battery = unit(battery->first / 5.0f);
            if (signal) sample_.signal = *signal;
            break;
        case CMD_INPUT_JOYSTICK:
            sample_.stickX = axis(packet.joystickData.joyX);
            sample_.stickY = axis(packet.joystickData.joyY);
            break;
        case CMD_ANGLE:
            std::transform(packet.result.begin(), packet.result.begin() + sample_.angles.size(), sample_.angles.begin(),
                             [](double value) { return static_cast<float>(value); });
            {
                auto offsets = packet.angleBoneOffset;
                const auto joints = jointRotations(offsets);
                for (std::size_t i = 0; i < joints.size(); ++i)
                    sample_.jointOffsets[i] = {joints[i]->x, joints[i]->y, joints[i]->z};
            }
            break;
        case CMD_INPUT_BUTTON: {
            const auto& b = packet.button;
            sample_.buttons = (b.btnA ? bit(Button::a) : 0) | (b.btnB ? bit(Button::b) : 0)
                | (b.btnMenu ? bit(Button::system) : 0) | (b.btnJoyStick ? bit(Button::stick) : 0)
                | (b.btnPower ? bit(Button::power) : 0) | (b.btnTrigger ? bit(Button::trigger) : 0)
                | (b.btnGrip ? bit(Button::grip) : 0) | (b.btnTrackpad ? bit(Button::pad) : 0);
            if (b.btnPower) ++sample_.calibrationRequests;
            sample_.trigger = unit(b.trigger);
            sample_.grip = unit(b.grip);
            sample_.pad = unit(b.trackpad);
            break;
        }
        case CMD_SKELETON_QUATERNION: {
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
    Millis fpsStart_{};
    int frames_{};
    std::string loadedSerial_;
    std::uint64_t loadedRevision_{};
    UdCapV1Core core_;
    std::function<void()> unlisten_;
};
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
        auto usb = UsbEnumerate::getInstance();
        usb->refresh(USB_ENUMERATE_REFRESH_SERIAL, isReceiver, true);
        const auto ports = usb->findPorts([](const SerialDevice& device) {
            return !device.isHid && isReceiver(device.vid, device.pid);
        });
        std::vector<std::shared_ptr<Device>> removed;
        {
            std::lock_guard lock(deviceMutex);
            for (auto it = devices.begin(); it != devices.end();) {
                if (std::ranges::none_of(ports, [&](const auto& port) { return port.portName == it->first; })) {
                    removed.push_back(std::move(it->second));
                    it = devices.erase(it);
                } else {
                    ++it;
                }
            }
        }
        removed.clear();
        for (const auto& port : ports) {
            {
                std::lock_guard lock(deviceMutex);
                if (devices.contains(port.portName)) continue;
            }
            try {
                auto accessor = PortAccessor::create(port);
                UdCapProbe probe(accessor);
                if (probe.probe() == UDCAP_PROBE_FAILURE) continue;
                auto device = std::make_shared<Device>(accessor, receiverSide(probe.getUDCapSerial()));
                std::lock_guard lock(deviceMutex);
                devices.emplace(port.portName, std::move(device));
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
                const auto side = device->side();
                if (side == UD_TARGET_UNKNOWN) continue;
                const auto index = side == UD_TARGET_LEFT_HAND ? 0u : 1u;
                device->restorePreferences(preferences, currentDefaults->hands[index],
                                           currentDefaults->revision);
            }
            for (const auto& command : pending) {
                if (stop.stop_requested()) break;
                const auto side = command.side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
                const auto device = std::ranges::find_if(devicesNow, [&](const auto& candidate) {
                    return candidate->side() == side;
                });
                if (device != devicesNow.end()) (*device)->command(command, preferences);
            }
        }
    }

    mutable std::mutex deviceMutex;
    std::map<std::string, std::shared_ptr<Device>> devices;
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
