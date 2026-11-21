#include "receiver.hpp"
#include "bridge.hpp"

#include <CorePref.h>
#include <UdCapProbe.h>
#include <UdCapV1Core.h>
#include <UsbEnumerate.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
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
    }

    ~Device() { if (unlisten_) unlisten_(); }

    UdTarget side() const { return core_.getTarget(); }

    Hand read() const {
        std::lock_guard lock(mutex_);
        return sample_;
    }

    void command(const Command& request) {
        try {
            auto stage = read().calibrationStep;
            if (request.action != Action::pairStart && request.action != Action::pairStop &&
                request.action != Action::pairBoth && request.action != Action::pairBothStop &&
                request.action != Action::channel && core_.getState() != UD_INIT_STATE_LINKED)
                throw std::runtime_error("Connect the glove first.");
            switch (request.action) {
            case Action::startHand:
                core_.runCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                stage = 1;
                break;
            case Action::fist:
                if (stage == 1) {
                    core_.captureCalibrationData(UDCAP_V1_HAND_CALI_TYPE_FIST);
                    stage = 2;
                }
                break;
            case Action::together:
                if (stage == 2) {
                    core_.captureCalibrationData(UDCAP_V1_HAND_CALI_TYPE_ADDUCTION);
                    stage = 3;
                }
                break;
            case Action::spread:
                if (stage == 3) {
                    core_.captureCalibrationData(UDCAP_V1_HAND_CALI_TYPE_PROTRACT);
                    core_.completeCalibration(UDCAP_V1_DEVICE_CALI_TYPE_HAND);
                    stage = 0;
                    save();
                }
                break;
            case Action::cancelHand:
                if (stage >= 1 && stage <= 3) {
                    core_.tryRestoreHandCalibration();
                    stage = 0;
                }
                break;
            case Action::joystickCenter:
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
                    save();
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
                if (std::isfinite(request.duration) && request.duration > 0 && unit(request.amplitude) > 0) {
                    core_.mcuSendVibration(1, request.duration, static_cast<int>(std::lround(10 * unit(request.amplitude))));
                }
                break;
            case Action::toggleStick:
            case Action::adjustPose: break;
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
    void save() {
        if (!core_.savePref()) throw std::runtime_error("Could not save glove calibration.");
    }

    void receive(const UdCapV1MCUPacket& packet) {
        if (packet.commandType == CMD_SERIAL || packet.commandType == CMD_LINK_STATE) {
            const auto serial = core_.getUDCapSerial();
            if (!serial.empty() && serial != loadedSerial_ && core_.getState() == UD_INIT_STATE_LINKED) {
                try {
                    core_.loadPref();
                    loadedSerial_ = serial;
                } catch (const std::exception& error) {
                    std::lock_guard lock(mutex_);
                    copyText(sample_.message, error.what());
                }
            }
        }
        const auto battery = packet.commandType == CMD_BATTERY ? core_.getBattery() : std::nullopt;
        const auto signal = packet.commandType == CMD_BATTERY ? core_.getRssiDbm() : std::nullopt;
        const bool calibrated = core_.getHandCalibrationStatus() == UDCAP_V1_HAND_CALI_STAT_COMPLETED;
        std::lock_guard lock(mutex_);
        sample_.calibrated = calibrated;
        switch (packet.commandType) {
        case CMD_PORT_CONNECTION_STATE:
            sample_ = {};
            sample_.receiver = packet.portConnectionState == PORT_ACCESSOR_CONNECTION_STATE_CONNECTED;
            break;
        case CMD_LINK_STATE:
            sample_.linked = packet.udState == UD_INIT_STATE_LINKED;
            if (!sample_.linked) {
                sample_.lastFrame = 0;
                sample_.calibrationStep = 0;
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
    UdCapV1Core core_;
    std::function<void()> unlisten_;
};
}

struct Receiver::Impl {
    explicit Impl(const std::filesystem::path& preferences) {
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
            {
                std::unique_lock lock(commandMutex);
                changed.wait_for(lock, stop, 30ms, [&] { return !commands.empty(); });
                pending.swap(commands);
            }
            for (const auto& command : pending) {
                if (stop.stop_requested()) break;
                const auto side = command.side == Side::left ? UD_TARGET_LEFT_HAND : UD_TARGET_RIGHT_HAND;
                const auto devicesNow = members();
                const auto device = std::ranges::find_if(devicesNow, [&](const auto& candidate) {
                    return candidate->side() == side;
                });
                if (device != devicesNow.end()) (*device)->command(command);
            }
        }
    }

    mutable std::mutex deviceMutex;
    std::map<std::string, std::shared_ptr<Device>> devices;
    std::mutex commandMutex;
    std::condition_variable_any changed;
    std::deque<Command> commands;
    std::jthread worker;
};

Receiver::Receiver(const std::filesystem::path& preferences) : impl_(std::make_unique<Impl>(preferences)) {}
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
}
