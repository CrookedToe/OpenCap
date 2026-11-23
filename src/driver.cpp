#include "bridge.hpp"
#include "vr_skeleton.hpp"
#include "vr_tracking.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace {
using namespace opencap;
using namespace vr;

constexpr std::array<const char*, 2> serials{"OpenCap-Left", "OpenCap-Right"};
constexpr std::array<const char*, 5> fingerPaths{
    "/input/finger/thumb/value", "/input/finger/index/value", "/input/finger/middle/value",
    "/input/finger/ring/value", "/input/finger/pinky/value"
};

struct BooleanInput { const char* path; Button button; bool touch{}; };
constexpr std::array booleanInputs{
    BooleanInput{"/input/a/click", Button::a},
    BooleanInput{"/input/b/click", Button::b},
    BooleanInput{"/input/system/click", Button::system},
    BooleanInput{"/input/joystick/click", Button::stick},
    BooleanInput{"/input/thumbstick/click", Button::stick},
    BooleanInput{"/input/trigger/click", Button::trigger},
    BooleanInput{"/input/trigger/touch", Button::trigger, true},
    BooleanInput{"/input/grip/click", Button::grip},
    BooleanInput{"/input/grip/touch", Button::grip, true},
    BooleanInput{"/input/grab/click", Button::grip},
    BooleanInput{"/input/pinch/click", Button::trigger},
    BooleanInput{"/input/trackpad/touch", Button::pad, true},
    BooleanInput{"/input/trackpad/click", Button::pad}
};

bool buttonValue(const BooleanInput& input, const Hand& hand, std::uint32_t buttons, bool joystickEnabled) {
    if (input.button == Button::stick && !joystickEnabled) return false;
    if (buttons & bit(input.button)) return true;
    if (!input.touch) return false;
    if (input.button == Button::trigger) return hand.trigger > 0;
    if (input.button == Button::grip) return hand.grip > 0;
    return hand.pad > 0;
}

using Poses = std::array<TrackedDevicePose_t, k_unMaxTrackedDeviceCount>;
using PoseClock = std::chrono::steady_clock;

unsigned collectTrackingSources(const Poses& poses, std::span<TrackingSource> sources) {
    unsigned count = 0;
    const auto properties = VRProperties();
    for (TrackedDeviceIndex_t i = 1; i < poses.size() && count < sources.size(); ++i) {
        if (!poses[i].bDeviceIsConnected) continue;
        const auto property = properties->TrackedDeviceToPropertyContainer(i);
        const auto type = properties->GetInt32Property(property, Prop_DeviceClass_Int32);
        if (type != TrackedDeviceClass_Controller && type != TrackedDeviceClass_GenericTracker) continue;
        TrackingSource source;
        auto& serial = source.device.serial;
        properties->GetStringProperty(property, Prop_SerialNumber_String, serial.data(), static_cast<uint32_t>(serial.size()));
        serial.back() = '\0';
        if (!serial[0] || std::strcmp(serial.data(), serials[0]) == 0 || std::strcmp(serial.data(), serials[1]) == 0)
            continue;
        source.device.index = i;
        source.role = static_cast<ETrackedControllerRole>(properties->GetInt32Property(property, Prop_ControllerRoleHint_Int32));
        source.tracker = type == TrackedDeviceClass_GenericTracker;
        source.valid = poses[i].bPoseIsValid && poses[i].eTrackingResult != TrackingResult_Fallback_RotationOnly;
        sources[count++] = source;
    }
    return count;
}

class Glove final : public ITrackedDeviceServerDriver {
public:
    explicit Glove(Side side) : side_(side), role_(side == Side::left
        ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand) {}

    const char* serial() const { return serials[static_cast<std::size_t>(side_)]; }

    EVRInitError Activate(TrackedDeviceIndex_t index) override {
        index_ = index;
        const auto p = VRProperties();
        const auto input = VRDriverInput();
        props_ = p->TrackedDeviceToPropertyContainer(index);
        p->SetStringProperty(props_, Prop_SerialNumber_String, serial());
        p->SetStringProperty(props_, Prop_ModelNumber_String, "OpenCap Glove");
        p->SetStringProperty(props_, Prop_ManufacturerName_String, "OpenCap");
        p->SetStringProperty(props_, Prop_ControllerType_String, "udcap");
        p->SetStringProperty(props_, Prop_InputProfilePath_String, "{opencap}/input/opencap_profile.json");
        p->SetStringProperty(props_, Prop_RenderModelName_String, side_ == Side::left
            ? "{indexcontroller}valve_controller_knu_1_0_left" : "{indexcontroller}valve_controller_knu_1_0_right");
        p->SetInt32Property(props_, Prop_ControllerRoleHint_Int32, role_);
        p->SetBoolProperty(props_, Prop_DeviceProvidesBatteryStatus_Bool, true);

        for (std::size_t i = 0; i < buttons_.size(); ++i)
            input->CreateBooleanComponent(props_, booleanInputs[i].path, &buttons_[i]);
        constexpr std::array axes{"/input/joystick/x", "/input/joystick/y", "/input/thumbstick/x", "/input/thumbstick/y"};
        for (std::size_t i = 0; i < axes.size(); ++i)
            input->CreateScalarComponent(props_, axes[i], &axes_[i], VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        constexpr std::array scalars{"/input/trigger/value", "/input/grip/value", "/input/grip/force", "/input/trackpad/force"};
        for (std::size_t i = 0; i < scalars.size(); ++i)
            input->CreateScalarComponent(props_, scalars[i], &analogs_[i], VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        for (std::size_t i = 0; i < fingers_.size(); ++i)
            input->CreateScalarComponent(props_, fingerPaths[i], &fingers_[i], VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        input->CreateHapticComponent(props_, "/output/haptic", &haptic_);
        input->CreateSkeletonComponent(props_, side_ == Side::left ? "/input/skeleton/left" : "/input/skeleton/right",
            side_ == Side::left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
            VRSkeletalTracking_Partial, nullptr, 0, &skeleton_);
        updateSkeleton(Hand{});
        return VRInitError_None;
    }

    void Deactivate() override { index_ = k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override { if (size) response[0] = 0; }

    DriverPose_t GetPose() override {
        std::lock_guard lock(poseMutex_);
        return lastPose_;
    }

    bool registered{};
    void update(const Hand& hand, const HandSettings& settings, bool live, const TrackedDevicePose_t* source,
                PoseClock::time_point sampledAt) {
        if (index_ == k_unTrackedDeviceIndexInvalid) return;
        const auto connected = live && settings.enabled;
        DriverPose_t pose;
        {
            std::lock_guard lock(poseMutex_);
            pose = makeTrackedPose(source, settings, connected,
                std::chrono::duration<double>(sampledAt - PoseClock::now()).count());
            lastPose_ = pose;
        }
        VRServerDriverHost()->TrackedDevicePoseUpdated(index_, pose, sizeof(pose));
        if (highPriority_ != settings.highPriority) {
            highPriority_ = settings.highPriority;
            VRProperties()->SetInt32Property(props_, Prop_ControllerHandSelectionPriority_Int32, highPriority_ ? 100 : 0);
        }
        if (!connected && !previouslyConnected_) return;
        previouslyConnected_ = connected;
        const Hand data = connected && !settings.skeletonOnly ? hand : Hand{};
        const auto mapped = remapButtons(data.buttons, settings.buttonMap);
        const auto input = VRDriverInput();
        for (std::size_t i = 0; i < buttons_.size(); ++i) {
            const auto pressed = buttonValue(booleanInputs[i], data, mapped, settings.joystick);
            if (buttons_[i]) input->UpdateBooleanComponent(buttons_[i], pressed, 0);
        }
        for (std::size_t i = 0; i < axes_.size(); ++i) {
            const auto value = i % 2 ? data.stickY : data.stickX;
            if (axes_[i]) input->UpdateScalarComponent(axes_[i], settings.joystick ? value : 0, 0);
        }
        const std::array values{data.trigger, data.grip, data.grip, data.pad};
        for (std::size_t i = 0; i < analogs_.size(); ++i)
            if (analogs_[i]) input->UpdateScalarComponent(analogs_[i], values[i], 0);
        for (std::size_t i = 0; i < fingers_.size(); ++i)
            if (fingers_[i]) input->UpdateScalarComponent(fingers_[i], data.curl[i], 0);
        if (connected && hand.battery >= 0 && hand.battery != battery_) {
            battery_ = hand.battery;
            VRProperties()->SetFloatProperty(props_, Prop_DeviceBatteryPercentage_Float, battery_);
        }
        updateSkeleton(connected ? hand : Hand{});
    }

    void haptic(const VREvent_HapticVibration_t& event, Bus& bus, bool enabled) const {
        if (!enabled || !haptic_ || event.componentHandle != haptic_ ||
            !std::isfinite(event.fAmplitude) || !std::isfinite(event.fDurationSeconds) ||
            event.fAmplitude <= 0 || event.fDurationSeconds <= 0) return;
        bus.send({side_, Action::haptic, 0, event.fDurationSeconds, event.fAmplitude, event.fFrequency});
    }

private:
    void updateSkeleton(const Hand& hand) {
        if (!skeleton_) return;
        const auto transforms = makeVrHandSkeleton(side_, hand);
        VRDriverInput()->UpdateSkeletonComponent(skeleton_, VRSkeletalMotionRange_WithController,
                                                  transforms.data(), static_cast<uint32_t>(transforms.size()));
        VRDriverInput()->UpdateSkeletonComponent(skeleton_, VRSkeletalMotionRange_WithoutController,
                                                  transforms.data(), static_cast<uint32_t>(transforms.size()));
    }

    Side side_;
    ETrackedControllerRole role_;
    TrackedDeviceIndex_t index_{k_unTrackedDeviceIndexInvalid};
    PropertyContainerHandle_t props_{};
    std::array<VRInputComponentHandle_t, booleanInputs.size()> buttons_{};
    std::array<VRInputComponentHandle_t, 4> axes_{};
    std::array<VRInputComponentHandle_t, 4> analogs_{};
    std::array<VRInputComponentHandle_t, fingerPaths.size()> fingers_{};
    VRInputComponentHandle_t skeleton_{}, haptic_{};
    std::mutex poseMutex_;
    DriverPose_t lastPose_{};
    float battery_{-1};
    bool previouslyConnected_{};
    bool highPriority_{};
};

class Provider final : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        bus_ = std::make_unique<Bus>();
        return VRInitError_None;
    }

    void Cleanup() override {
        for (auto& glove : gloves_) glove.reset();
        if (bus_) bus_->publishTracking({});
        bus_.reset();
        bindings_ = {};
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

    void RunFrame() override {
        if (const auto updated = bus_->read()) snapshot_ = *updated;
        Poses poses{};
        const auto readStarted = PoseClock::now();
        VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses.data(), static_cast<uint32_t>(poses.size()));
        const auto sampledAt = readStarted + (PoseClock::now() - readStarted) / 2;
        std::array<TrackingSource, k_unMaxTrackedDeviceCount - 1> available{};
        const auto sources = std::span{available}.first(collectTrackingSources(poses, available));
        const auto time = now();
        auto tracking = std::array<std::uint32_t, handCount>{};
        tracking.fill(noTracker);
        for (std::size_t i = 0; i < gloves_.size(); ++i) {
            const auto side = static_cast<Side>(i);
            const auto& settings = snapshot_.settings[i];
            const bool active = isLive(snapshot_, side, time);
            if (active && settings.enabled) {
                tracking[i] = bindings_[i].resolve(side, settings, sources);
                if (!gloves_[i]) {
                    auto device = std::make_unique<Glove>(side);
                    if (VRServerDriverHost()->TrackedDeviceAdded(device->serial(), TrackedDeviceClass_Controller, device.get()))
                        gloves_[i] = std::move(device);
                }
            }
            if (gloves_[i]) gloves_[i]->update(snapshot_.hands[i], settings, active,
                tracking[i] < poses.size() ? &poses[tracking[i]] : nullptr, sampledAt);
        }
        if (time - lastTrackingUpdate_ >= 250) publishTracking(sources, tracking, time);
        VREvent_t event{};
        while (VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType != VREvent_Input_HapticVibration) continue;
            for (std::size_t i = 0; i < gloves_.size(); ++i) {
                const auto& settings = snapshot_.settings[i];
                if (gloves_[i]) gloves_[i]->haptic(event.data.hapticVibration, *bus_,
                    settings.enabled && !settings.skeletonOnly && settings.haptics && isLive(snapshot_, static_cast<Side>(i), time));
            }
        }
    }

private:
    void publishTracking(std::span<const TrackingSource> sources,
                         const std::array<std::uint32_t, handCount>& selected, Millis time) {
        TrackingSnapshot status;
        status.heartbeat = time;
        status.selected = selected;
        for (const auto& source : sources) status.devices[status.count++] = source.device;
        if (bus_->publishTracking(status)) lastTrackingUpdate_ = time;
    }

    std::unique_ptr<Bus> bus_;
    Snapshot snapshot_{};
    std::array<std::unique_ptr<Glove>, handCount> gloves_;
    std::array<TrackerBinding, handCount> bindings_{};
    Millis lastTrackingUpdate_{};
};

Provider provider;
}

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* version, int* code) {
    if (version && std::strcmp(version, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (code) *code = vr::VRInitError_None;
        return &provider;
    }
    if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
