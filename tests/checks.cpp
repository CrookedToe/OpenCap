#include "bridge.hpp"
#include "osc_compat.hpp"

#include <windows.h>
#include <openvr_driver.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace opencap;

static void require(bool result, const char* description) {
    if (!result) throw std::runtime_error(description);
}

static void checkDriver(const wchar_t* path) {
    const auto library = LoadLibraryW(path);
    require(library != nullptr, "Load the built SteamVR driver");
    using Factory = void* (*)(const char*, int*);
    const auto factory = reinterpret_cast<Factory>(GetProcAddress(library, "HmdDriverFactory"));
    const bool exported = factory != nullptr;
    int code = 0;
    const bool rejects = exported && !factory("invalid", &code) && code == vr::VRInitError_Init_InterfaceNotFound;
    const bool accepts = exported && factory(vr::IServerTrackedDeviceProvider_Version, &code) && code == vr::VRInitError_None;
    FreeLibrary(library);
    require(accepts && rejects, "Driver exports the SteamVR factory and validates the interface");
}

class TestDesktop {
public:
    explicit TestDesktop(const wchar_t* path, DWORD owner = 0) {
        auto arguments = L"\"" + std::wstring(path) + L"\" --demo --test-session " + id();
        if (owner) arguments += L" --steamvr-session " + std::to_wstring(owner);
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        start.dwFlags = STARTF_USESHOWWINDOW;
        start.wShowWindow = SW_HIDE;
        require(CreateProcessW(path, arguments.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                               &start, &process_) != FALSE, "Start the isolated desktop demo");
    }

    static std::wstring id() { return std::to_wstring(GetCurrentProcessId()); }
    bool exited() const { return WaitForSingleObject(process_.hProcess, 3000) == WAIT_OBJECT_0; }

    ~TestDesktop() {
        EnumThreadWindows(process_.dwThreadId, closeWindow, 0);
        if (WaitForSingleObject(process_.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(process_.hProcess, 1);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
    }

private:
    static BOOL CALLBACK closeWindow(HWND window, LPARAM) {
        PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
    }
    PROCESS_INFORMATION process_{};
};

class TestOwner {
public:
    explicit TestOwner(const wchar_t* helper) {
        auto name = L"Local\\OpenCap.Lifetime." + TestDesktop::id();
        stop_ = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        require(stop_ != nullptr, "Create a disposable overlay lifetime signal");
        auto arguments = L"\"" + std::wstring(helper) + L"\" --wait-as-owner " + name;
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        if (!CreateProcessW(helper, arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &start, &process_)) {
            CloseHandle(stop_);
            throw std::runtime_error("Start a disposable overlay process");
        }
    }
    ~TestOwner() {
        stop();
        CloseHandle(stop_);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
    }
    void stop() {
        SetEvent(stop_);
        WaitForSingleObject(process_.hProcess, 3000);
    }
    DWORD id() const { return process_.dwProcessId; }
private:
    HANDLE stop_{};
    PROCESS_INFORMATION process_{};
};

static void checkDemo(const wchar_t* path, TestOwner* owner = nullptr) {
    const TestDesktop app(path, owner ? owner->id() : 0);
    Bus bus(std::wstring(sessionName) + L".Checks." + app.id());
    const auto waitFor = [&](const auto& condition) {
        const auto deadline = now() + 3000;
        do {
            if (const auto sample = bus.read(10); sample && condition(*sample)) return true;
            Sleep(20);
        } while (now() < deadline);
        return false;
    };
    require(waitFor([](const auto& sample) {
        const auto t = now();
        return sample.demo && isLive(sample, Side::left, t) && isLive(sample, Side::right, t);
    }), "Desktop publishes two simulated gloves");
    if (owner) {
        owner->stop();
        require(app.exited(), "A desktop started by SteamVR closes when its overlay exits");
        require(waitFor([](const auto& sample) { return sample.heartbeat == 0; }),
                "An overlay-managed desktop clears its published state on shutdown");
        return;
    }

    const std::array steps{Action::startHand, Action::fist, Action::together, Action::spread};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        require(bus.send({Side::right, steps[i]}), "Send a calibration command to the desktop");
        require(waitFor([i](const auto& sample) {
            return sample.hands[1].calibrationStep == (i + 1) % 4 && sample.hands[0].calibrationStep == 0;
        }), "Calibration advances on the requested hand");
    }
    require(bus.send({Side::right, Action::setJointOffset, 3, 12, -10, 5}), "Send a joint correction to the desktop");
    require(waitFor([](const auto& sample) {
        const auto right = sample.hands[1].jointOffsets[3];
        return right.x == 12 && right.y == -10 && right.z == 5 && sample.hands[0].jointOffsets[3].x == 0;
    }), "Joint correction applies only to the requested hand");
    require(bus.send({Side::right, Action::togglePublishing}) && bus.send({Side::right, Action::toggleHaptics}),
            "Send dashboard controller toggles");
    require(waitFor([](const auto& sample) {
        return !sample.settings[1].enabled && !sample.settings[1].haptics && sample.settings[0].enabled;
    }), "Dashboard capability toggles apply to one hand");
    TrackingSnapshot tracking;
    tracking.heartbeat = now();
    tracking.count = 2;
    tracking.devices[0].index = 7;
    tracking.devices[1].index = 8;
    const std::string_view tracker1 = "QA-TRACKER-ONE", tracker2 = "QA-TRACKER-TWO";
    std::copy(tracker1.begin(), tracker1.end(), tracking.devices[0].serial.begin());
    std::copy(tracker2.begin(), tracker2.end(), tracking.devices[1].serial.begin());
    require(bus.publishTracking(tracking), "Publish connected trackers");
    for (const auto expected : {tracker1, tracker2, std::string_view{}}) {
        require(bus.send({Side::right, expected.empty() ? Action::autoTracker : Action::cycleTracker}),
                "Choose the next tracker or restore automatic selection");
        require(waitFor([expected](const auto& sample) {
            return sample.settings[1].tracker == noTracker && expected == sample.settings[1].trackerSerial.data();
        }), "Dashboard stores the selected tracker's serial");
    }
    require(bus.send({Side::left, Action::pairBoth}) &&
            waitFor([](const auto& sample) { return sample.hands[0].pairing && sample.hands[1].pairing; }),
            "Start pairing for both gloves");
    require(bus.send({Side::left, Action::pairBothStop}) &&
            waitFor([](const auto& sample) { return !sample.hands[0].pairing && !sample.hands[1].pairing; }),
            "Stop pairing for both gloves");
    require(bus.send({Side::right, Action::channel, 20}) && bus.send({Side::left, Action::channel, 23}) &&
            waitFor([](const auto& sample) { return sample.hands[0].channel == 23 && sample.hands[1].channel == 40; }),
            "Apply a free radio channel and reject a channel in use by the other glove");
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--driver") { checkDriver(argv[2]); return 0; }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--desktop") { checkDemo(argv[2]); return 0; }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--wait-as-owner") {
            const auto stop = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
            if (!stop) return 1;
            const auto result = WaitForSingleObject(stop, 10000);
            CloseHandle(stop);
            return result == WAIT_OBJECT_0 ? 0 : 1;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--desktop-lifetime") {
            TestOwner owner(argv[0]);
            checkDemo(argv[2], &owner);
            return 0;
        }
        const auto name = L"Local\\OpenCap.Checks." + std::to_wstring(GetCurrentProcessId());
        Bus writer(name), reader(name);
        require(writer.valid() && reader.valid(), "Create shared session");
        Snapshot sample;
        sample.heartbeat = 2000;
        sample.hands[0] = demoHand(Side::left, 2000);
        require(writer.publish(sample), "Publish a snapshot");
        const auto seen = reader.read(100);
        require(seen && isLive(*seen, Side::left, 2200), "Snapshot crosses the session boundary");
        require(!isLive(*seen, Side::right, 2200), "Inactive hand stays disconnected");
        require(!isLive(*seen, Side::left, 2501), "Expired input is released");
        require(!isLive(*seen, Side::left, 1999), "Future timestamps are rejected");
        TrackingSnapshot tracker;
        tracker.heartbeat = 2000;
        tracker.selected[1] = 23;
        require(writer.publishTracking(tracker) && reader.readTracking()->selected[1] == 23
            && trackingCurrent(tracker, 2999) && !trackingCurrent(tracker, 4000),
            "Driver pose-source telemetry is shared and expires when the runtime stops");

        for (int i = 0; i < 32; ++i)
            require(reader.send({Side::right, Action::channel, i}), "Queue accepts commands");
        require(!reader.send({Side::right, Action::channel, 32}), "Full queue preserves prior commands");
        for (int i = 0; i < 32; ++i) {
            const auto command = writer.next();
            require(command && command->side == Side::right && command->value == i, "Queue preserves order");
        }
        require(!writer.next(), "Consumed commands are not repeated");
        require(reader.send({Side::left, Action::toggleStick}), "Queue can wrap");
        require(writer.next()->action == Action::toggleStick, "Wrapped command is readable");
        require(reader.send({Side::left, Action::startHand}) && writer.reset(), "Restart the desktop session");
        require(!writer.next() && reader.read(100)->heartbeat == 0, "Restart clears commands and stale input");

        const auto point = rotate(eulerDegrees({0, 0, 90}), {1, 0, 0});
        require(std::abs(point.x) < 1e-5f && std::abs(point.y - 1) < 1e-5f, "Rotation is right-handed");
        require(normalize({0, 0, 0, 0}).w == 1.0f, "Invalid quaternion resets to identity");
        require(validJointOffset({45, -45, 0}) && !validJointOffset({0, 46, 0}),
                "Joint corrections enforce the supported angle limit");
        require(validButtonRange({.2f, .5f}) && !validButtonRange({.6f, .5f}) && !validButtonRange({.1f, 1.2f}),
                "Touch and click thresholds must be ordered and normalized");
        CalibrationSamples observations{};
        observations[0][0] = -38;
        observations[1][0] = 17;
        observations[2][0] = 200;
        observations[0][4] = -310;
        observations[2][4] = 49;
        observations[0][5] = 25;
        observations[0][12] = 1000;
        observations[1][12] = 2000;
        const auto spans = calibrationSpans(observations);
        require(spans.measured && spans.counts[0] == 55 && spans.counts[4] == 49 &&
                spans.counts[5] == 25 && spans.lowChannels == 10,
                "Use the corresponding bend and splay poses and exclude auxiliary inputs from quality");
        std::ostringstream report;
        writeCalibrationReport(report, Side::right, observations);
        require(report.view().find("right,flexion,4,-38,17,200,55,measured") != std::string_view::npos &&
                report.view().find("right,splay,8,-310,0,49,49,measured") != std::string_view::npos &&
                report.view().find("right,auxiliary,1,1000,2000,0,,") != std::string_view::npos,
                "Export the selected hand's original sensor captures and movement readings as CSV");
        StickHold hold;
        auto alignment = std::array<HandSettings, handCount>{};
        alignment[0].tracker = 8;
        require(applyTrackingPreset(alignment, 1) && alignment[0].tracker == 8
            && alignment[0].rotation.y == -60 && alignment[1].rotation.y == 60
            && alignment[0].position.z == .04f && !applyTrackingPreset(alignment, 100),
            "Tracking presets preserve bindings and apply both hand transforms");
        require(!hold.update(true, 1000) && !hold.update(true, 5999) && hold.update(true, 6000),
                "Holding the physical stick toggles its output at five seconds");
        require(!hold.update(true, 9000) && !hold.update(false, 9010) && !hold.update(true, 10000) && hold.update(true, 15000),
                "A second toggle requires release and another hold");
        const ButtonMap mapping{1, 0, disabledButton, 3};
        require(remapButtons(bit(Button::a) | bit(Button::system) | bit(Button::grip), mapping)
                    == (bit(Button::b) | bit(Button::grip)), "Input mapping swaps and disables selected outputs");
        require(avatarBits(.5f) == 2 && avatarBits(.25f) == 4 && avatarBits(.1f) == 10,
                "Avatar bits select the nearest reciprocal");
        Snapshot avatar;
        avatar.hands[1].angles[15] = 6;
        const auto parameters = avatarParameters(avatar, Side::left);
        require(parameters[11].control == "Ring1spread" && parameters[11].bits == 2,
                "Ring spread preserves first-party channel selection");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
