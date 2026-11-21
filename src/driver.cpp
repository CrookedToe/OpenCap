#include "bridge.hpp"
#include "vr_skeleton.hpp"

#include <algorithm>
#include <array>
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

Quaternion matrixRotation(const HmdMatrix34_t& pose) {
    const auto& m = pose.m;
    const auto trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const auto scale = 2 * std::sqrt(trace + 1);
        return normalize({(m[2][1]-m[1][2])/scale, (m[0][2]-m[2][0])/scale,
                          (m[1][0]-m[0][1])/scale, scale/4});
    }
    unsigned i = 0;
    if (m[1][1] > m[i][i]) i = 1;
    if (m[2][2] > m[i][i]) i = 2;
    const auto j = (i + 1) % 3;
    const auto k = (i + 2) % 3;
    const auto scale = 2 * std::sqrt((std::max)(0.0f, 1 + m[i][i] - m[j][j] - m[k][k]));
    if (scale < 1e-6f) return {};
    float v[3]{};
    v[i] = scale/4;
    v[j] = (m[j][i] + m[i][j])/scale;
    v[k] = (m[k][i] + m[i][k])/scale;
    return normalize({v[0], v[1], v[2], (m[k][j] - m[j][k])/scale});
}

bool trackerSerial(TrackedDeviceIndex_t index, char (&serial)[128]) {
    const auto properties = VRProperties()->TrackedDeviceToPropertyContainer(index);
    const auto type = VRProperties()->GetInt32Property(properties, Prop_DeviceClass_Int32);
    if (type != TrackedDeviceClass_Controller && type != TrackedDeviceClass_GenericTracker) return false;
    VRProperties()->GetStringProperty(properties, Prop_SerialNumber_String, serial, sizeof(serial));
    serial[sizeof(serial) - 1] = '\0';
    return std::strcmp(serial, serials[0]) && std::strcmp(serial, serials[1]);
}

using Poses = std::array<TrackedDevicePose_t, k_unMaxTrackedDeviceCount>;

TrackedDeviceIndex_t findTracker(Side side, const HandSettings& settings, const Poses& poses) {
    if (settings.tracker != noTracker) {
        const auto index = settings.tracker;
        char serial[128]{};
        return index < poses.size() && poses[index].bDeviceIsConnected && poses[index].bPoseIsValid &&
               trackerSerial(index, serial) ? index : k_unTrackedDeviceIndexInvalid;
    }
    const auto wanted = side == Side::left ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand;
    for (TrackedDeviceIndex_t i = 1; i < poses.size(); ++i) {
        char serial[128]{};
        if (!poses[i].bDeviceIsConnected || !poses[i].bPoseIsValid || !trackerSerial(i, serial)) continue;
        const auto properties = VRProperties()->TrackedDeviceToPropertyContainer(i);
        if (settings.trackerSerial[0]) {
            if (std::strcmp(serial, settings.trackerSerial.data()) == 0) return i;
        } else if (VRProperties()->GetInt32Property(properties, Prop_ControllerRoleHint_Int32) == wanted) {
            return i;
        }
    }
    return k_unTrackedDeviceIndexInvalid;
}

DriverPose_t makePose(TrackedDeviceIndex_t index, const HandSettings& settings, bool connected, const Poses& poses) {
    DriverPose_t output{};
    output.qWorldFromDriverRotation.w = output.qDriverFromHeadRotation.w = output.qRotation.w = 1;
    output.deviceIsConnected = connected;
    output.poseTimeOffset = settings.poseTimeOffset;
    output.result = connected ? TrackingResult_Running_OutOfRange : TrackingResult_Uninitialized;
    if (!connected) return output;
    if (index == k_unTrackedDeviceIndexInvalid) return output;
    const auto& tracker = poses[index];
    const auto& matrix = tracker.mDeviceToAbsoluteTracking;
    const auto trackerRotation = matrixRotation(matrix);
    const auto r = normalize(multiply(trackerRotation, eulerDegrees(settings.rotation)));
    const auto offset = rotate(trackerRotation, settings.position);
    output.qRotation = {r.w, r.x, r.y, r.z};
    output.vecPosition[0] = matrix.m[0][3] + offset.x;
    output.vecPosition[1] = matrix.m[1][3] + offset.y;
    output.vecPosition[2] = matrix.m[2][3] + offset.z;
    const auto& angular = tracker.vAngularVelocity.v;
    output.vecVelocity[0] = tracker.vVelocity.v[0] + angular[1]*offset.z - angular[2]*offset.y;
    output.vecVelocity[1] = tracker.vVelocity.v[1] + angular[2]*offset.x - angular[0]*offset.z;
    output.vecVelocity[2] = tracker.vVelocity.v[2] + angular[0]*offset.y - angular[1]*offset.x;
    std::copy_n(angular, 3, output.vecAngularVelocity);
    output.poseIsValid = true;
    output.result = TrackingResult_Running_OK;
    return output;
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
    void update(const Hand& hand, const HandSettings& settings, bool live, const Poses& poses, TrackedDeviceIndex_t source) {
        if (index_ == k_unTrackedDeviceIndexInvalid) return;
        const auto connected = live && settings.enabled;
        const auto pose = makePose(source, settings, connected, poses);
        {
            std::lock_guard lock(poseMutex_);
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
        if (connected && data.battery >= 0 && data.battery != battery_) {
            battery_ = data.battery;
            VRProperties()->SetFloatProperty(props_, Prop_DeviceBatteryPercentage_Float, battery_);
        }
        updateSkeleton(connected ? hand : Hand{});
    }

    void haptic(const VREvent_HapticVibration_t& event, Bus& bus, bool enabled) const {
        if (!enabled || !haptic_ || event.componentHandle != haptic_ ||
            !std::isfinite(event.fAmplitude) || !std::isfinite(event.fDurationSeconds) ||
            event.fAmplitude <= 0 || event.fDurationSeconds <= 0) return;
        bus.send({side_, Action::haptic, 0, event.fDurationSeconds, event.fAmplitude});
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
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

    void RunFrame() override {
        if (const auto updated = bus_->read()) snapshot_ = *updated;
        Poses poses{};
        VRServerDriverHost()->GetRawTrackedDevicePoses(0, poses.data(), static_cast<uint32_t>(poses.size()));
        const auto time = now();
        auto tracking = std::array<std::uint32_t, handCount>{};
        for (std::size_t i = 0; i < gloves_.size(); ++i) {
            const auto side = static_cast<Side>(i);
            tracking[i] = findTracker(side, snapshot_.settings[i], poses);
            const bool active = isLive(snapshot_, side, time);
            if (active && snapshot_.settings[i].enabled && !gloves_[i]) {
                auto device = std::make_unique<Glove>(side);
                if (VRServerDriverHost()->TrackedDeviceAdded(device->serial(), TrackedDeviceClass_Controller, device.get()))
                    gloves_[i] = std::move(device);
            }
            if (gloves_[i]) gloves_[i]->update(snapshot_.hands[i], snapshot_.settings[i], active, poses, tracking[i]);
        }
        if (time - lastTrackingUpdate_ >= 250) publishTracking(poses, tracking, time);
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
    void publishTracking(const Poses& poses, const std::array<std::uint32_t, handCount>& selected, Millis time) {
        TrackingSnapshot status;
        status.heartbeat = time;
        status.selected = selected;
        for (TrackedDeviceIndex_t i = 1; i < poses.size() && status.count < status.devices.size(); ++i) {
            char serial[128]{};
            if (!poses[i].bDeviceIsConnected || !trackerSerial(i, serial) || !serial[0]) continue;
            auto& entry = status.devices[status.count++];
            entry.index = i;
            std::copy_n(serial, (std::min)(std::strlen(serial), entry.serial.size() - 1), entry.serial.data());
        }
        if (bus_->publishTracking(status)) lastTrackingUpdate_ = time;
    }

    std::unique_ptr<Bus> bus_;
    Snapshot snapshot_{};
    std::array<std::unique_ptr<Glove>, handCount> gloves_;
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
