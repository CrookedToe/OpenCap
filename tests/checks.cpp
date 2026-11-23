#include <winsock2.h>
#include "bridge.hpp"
#include "osc_compat.hpp"
#include "stream.hpp"

#include <windows.h>
#include <openvr_driver.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace opencap;

static void require(bool result, const char* description) {
    if (!result) throw std::runtime_error(description);
}

static int checkVmc() {
    struct Listener {
        SOCKET socket{INVALID_SOCKET};
        bool started{};
        ~Listener() {
            if (socket != INVALID_SOCKET) closesocket(socket);
            if (started) WSACleanup();
        }
    } listener;
    WSADATA library{};
    listener.started = WSAStartup(MAKEWORD(2, 2), &library) == 0;
    require(listener.started, "Start Winsock for the VMC loopback test");
    listener.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(listener.socket != INVALID_SOCKET, "Open the VMC loopback listener");
    DWORD timeout = 500;
    int buffer = 256 * 1024;
    require(setsockopt(listener.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
            setsockopt(listener.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer)) == 0,
            "Configure the VMC loopback listener");
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int endpointLength = sizeof(endpoint);
    require(bind(listener.socket, reinterpret_cast<sockaddr*>(&endpoint), endpointLength) == 0 &&
            getsockname(listener.socket, reinterpret_cast<sockaddr*>(&endpoint), &endpointLength) == 0,
            "Bind an ephemeral VMC port");
    {
        Listener probe;
        probe.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(probe.socket != INVALID_SOCKET, "Open the VMC loopback probe");
        char received{};
        if (sendto(probe.socket, "x", 1, 0, reinterpret_cast<const sockaddr*>(&endpoint), endpointLength) != 1 ||
            recv(listener.socket, &received, 1, 0) != 1 || received != 'x') {
            std::cerr << "This environment does not deliver UDP loopback packets.\n";
            return 77;
        }
    }

    Stream stream;
    StreamSettings settings;
    settings.vmcPort = ntohs(endpoint.sin_port);
    require(stream.configure(settings), "Configure the VMC destination");
    Snapshot sample;
    sample.heartbeat = 1000;
    auto& left = sample.hands[0];
    left.lastFrame = sample.heartbeat;
    left.receiver = left.linked = left.calibrated = true;
    left.stickX = -.75f;
    left.angles[6] = -90;
    stream.send(sample);
    const auto readFrame = [&](float stick, float curl) {
        bool sawBone = false, sawBlend = false, sawNeutral = false;
        for (unsigned index = 0; index < 72; ++index) {
            std::array<char, 256> bytes{};
            const auto count = recv(listener.socket, bytes.data(), static_cast<int>(bytes.size()), 0);
            require(count > 0, "Receive the complete VMC frame over UDP");
            std::string_view unread(bytes.data(), count);
            const auto word = [&] {
                const auto end = unread.find('\0');
                require(end != unread.npos && ((end + 4) & ~std::size_t{3}) <= unread.size(), "Read a padded OSC string");
                const auto value = unread.substr(0, end);
                unread.remove_prefix((end + 4) & ~std::size_t{3});
                return value;
            };
            const auto value = [&](std::size_t component) {
                std::uint32_t bits{};
                require(unread.size() >= (component + 1) * sizeof(bits), "Read an OSC float");
                std::memcpy(&bits, unread.data() + component * sizeof(bits), sizeof(bits));
                return std::bit_cast<float>(ntohl(bits));
            };
            const auto address = word();
            const auto types = word();
            if (index == 71) require(address == "/VMC/Ext/Blend/Apply" && types == ",", "Finish the VMC frame with Blend Apply");
            else if (address == "/VMC/Ext/Bone/Pos" && types == ",sfffffff") {
                const auto name = word();
                if (name == "LeftIndexProximal") sawBone = std::abs(value(5) - curl) < 1e-5f;
                if (name == "RightIndexProximal") sawNeutral = value(5) == 0 && value(6) == 1;
            } else if (address == "/VMC/Ext/Blend/Val" && types == ",sf" && word() == "LeftJoyX_Negative")
                sawBlend = value(0) == stick;
        }
        require(sawBone && sawBlend && sawNeutral, "Send finger rotations, controller blends, and disconnected-hand resets");
    };
    readFrame(.75f, std::sqrt(.5f));
    sample.heartbeat += sampleTimeout + 1;
    stream.send(sample);
    readFrame(0, 0);
    return 0;
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

    bool click(const wchar_t* label) const {
        const auto control = button(label);
        DWORD_PTR handled{};
        return control && IsWindowEnabled(control) && SendMessageTimeoutW(GetParent(control), WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(control), BN_CLICKED), reinterpret_cast<LPARAM>(control),
            SMTO_ABORTIFHUNG, 1000, &handled);
    }

    bool shows(const wchar_t* label) const {
        const auto control = button(label);
        return control && (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE);
    }

    ~TestDesktop() {
        EnumThreadWindows(process_.dwThreadId, closeWindow, 0);
        if (WaitForSingleObject(process_.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(process_.hProcess, 1);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
    }

private:
    HWND button(const wchar_t* label) const {
        HWND desktop{};
        EnumThreadWindows(process_.dwThreadId, [](HWND window, LPARAM result) -> BOOL {
            wchar_t name[32]{};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (std::wstring_view(name) != L"OpenCapDesktop") return TRUE;
            *reinterpret_cast<HWND*>(result) = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&desktop));
        return desktop ? FindWindowExW(desktop, nullptr, L"Button", label) : nullptr;
    }

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
    const auto waitFor = [&](const auto& condition, Millis timeout = 3000) {
        const auto deadline = now() + timeout;
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

    require(app.shows(L"Calibrate both gloves") && !app.shows(L"Pair glove"), "Open on the Gloves page");
    require(app.click(L"Devices") && app.shows(L"Pair glove") && !app.shows(L"Calibrate both gloves"),
            "Navigate to pairing and hide calibration controls");
    require(app.click(L"Tracking") && app.shows(L"Apply tracker alignment") && !app.shows(L"Pair glove"),
            "Navigate to tracker settings");
    require(app.click(L"Streaming") && app.shows(L"Apply streaming") && !app.shows(L"Apply tracker alignment"),
            "Navigate to streaming settings");
    require(app.click(L"Settings") && app.shows(L"Inspect firmware image…") && !app.shows(L"Apply streaming"),
            "Navigate to maintenance settings");
    require(app.click(L"Gloves") && app.shows(L"Calibrate both gloves") && app.click(L"Calibrate both gloves"),
            "Return to Gloves and start guided calibration");
    for (unsigned pose = 1; pose <= 3; ++pose) {
        require(waitFor([pose](const auto& sample) {
            const auto& guide = sample.calibrationGuide;
            return guide.hands == 3 && guide.phase == pose && guide.captureAt >= sample.heartbeat + 2000 &&
                sample.hands[0].calibrationStep == pose && sample.hands[1].calibrationStep == pose;
        }, 4500), "Give the user a countdown for each pose before recording and advancing");
        if (pose == 1) {
            require(bus.send({Side::left, Action::fist}), "Queue an unrelated manual capture");
            Sleep(200);
            const auto sample = bus.read(50);
            require(sample && sample->hands[0].calibrationStep == 1,
                    "Keep the countdown active when an unrelated capture arrives");
        }
    }
    require(waitFor([](const auto& sample) {
        return !sample.calibrationGuide.hands && sample.calibrationGuide.result == CalibrationResult::saved &&
            sample.hands[0].calibrationResult == CalibrationResult::saved && sample.hands[1].calibrationResult == CalibrationResult::saved;
    }, 4500), "Record and save the final pose for both gloves automatically");
    require(bus.send({Side::right, Action::joystickCenter}) && waitFor([](const auto& sample) {
        return sample.hands[1].calibrationStep == 4;
    }), "Enter joystick calibration on the right glove");
    require(bus.send({Side::left, Action::guidedCalibration}) && waitFor([](const auto& sample) {
        return !sample.calibrationGuide.hands && sample.calibrationGuide.result == CalibrationResult::failed &&
            sample.hands[0].calibrationStep == 0 && sample.hands[1].calibrationStep == 4;
    }), "A busy glove blocks the whole hand calibration session");
    require(bus.send({Side::right, Action::joystickFinish}) && waitFor([](const auto& sample) {
        return sample.hands[1].calibrationStep == 0;
    }), "Finish joystick calibration before starting both gloves");
    require(bus.send({Side::right, Action::guidedCalibration}) && waitFor([](const auto& sample) {
        return sample.calibrationGuide.hands == 3 && sample.calibrationGuide.captureAt != 0;
    }), "A dashboard calibration command always starts both gloves");
    require(bus.send({Side::left, Action::cancelHand}) && waitFor([](const auto& sample) {
        return !sample.calibrationGuide.hands && sample.calibrationGuide.result == CalibrationResult::canceled &&
            sample.calibrationGuide.message[0] && sample.hands[0].calibrationResult == CalibrationResult::canceled &&
            sample.hands[1].calibrationResult == CalibrationResult::canceled;
    }), "Cancel the shared calibration for both gloves");

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
        if (argc == 2 && std::wstring_view(argv[1]) == L"--vmc-loopback") return checkVmc();
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
        require(validHandCurve(0, {-80, 5, .6, 4}) && validHandCurve(11, {0, 0, .5, 8}) &&
                !validHandCurve(11, {-5, 5, .5, 0}) && !validHandCurve(0, {-90, 0, 1.1, 0}),
                "Validate per-channel hand response and restrict middle splay to an offset");
        CalibrationSamples observations{};
        observations[0][0] = -38;
        observations[1][0] = 17;
        observations[2][0] = 200;
        observations[0][3] = 700;
        observations[0][4] = -310;
        observations[2][4] = 49;
        observations[0][5] = 25;
        observations[0][12] = 1000;
        observations[1][12] = 2000;
        const auto spans = calibrationSpans(observations);
        require(spans.measured && spans.counts[0] == 55 && spans.counts[4] == 49 &&
                spans.counts[5] == 25 && spans.lowChannels == 11,
                "Use the corresponding bend and splay poses and exclude auxiliary inputs from quality");
        require(calibrationGrade(0, 419) == CalibrationGrade::poor && calibrationGrade(0, 420) == CalibrationGrade::average &&
                calibrationGrade(0, 700) == CalibrationGrade::good && calibrationGrade(0, 1050) == CalibrationGrade::excellent &&
                calibrationGrade(4, 129) == CalibrationGrade::poor && calibrationGrade(4, 130) == CalibrationGrade::average &&
                calibrationGrade(4, 220) == CalibrationGrade::good && calibrationGrade(4, 350) == CalibrationGrade::excellent,
                "Calibration grades use the reference per-channel sensor movement boundaries");
        Hand signal;
        signal.signal = -44;
        require(radioSignalLabel(signal, true) == L"Strong (-44 dBm)" &&
                radioSignalLabel(signal, false) == L"No signal reading", "Show link quality only for a live glove");
        signal.signal = -45;
        require(radioSignalLabel(signal, true) == L"Fair (-45 dBm)", "Radio uses the reference strong-signal threshold");
        signal.signal = -60;
        require(radioSignalLabel(signal, true) == L"Weak (-60 dBm)", "Radio uses the reference weak-signal threshold");
        std::ostringstream report;
        writeCalibrationReport(report, Side::right, observations);
        require(report.view().find("right,flexion,4,-38,17,200,55,Poor") != std::string_view::npos &&
                report.view().find("right,flexion,7,700,0,0,700,Excellent") != std::string_view::npos &&
                report.view().find("right,splay,8,-310,0,49,49,Poor") != std::string_view::npos &&
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
        Hand vmc;
        vmc.angles[6] = -90;
        vmc.angles[1] = 60;
        const auto vmcLeft = vmcFingerRotations(vmc, Side::left);
        const auto vmcRight = vmcFingerRotations(vmc, Side::right);
        require(std::abs(vmcLeft[0].z - std::sqrt(.5f)) < 1e-5f &&
                std::abs(vmcLeft[0].z + vmcRight[0].z) < 1e-5f &&
                std::abs(vmcLeft[13].y - .5f) < 1e-5f && std::abs(vmcRight[13].y + .5f) < 1e-5f,
                "VMC uses mirrored finger bending and rotates thumb segments around Y");
        vmc.stickX = -.7f;
        vmc.stickY = .2f;
        vmc.buttons = bit(Button::a) | bit(Button::system);
        const auto blend = vmcControllerInputs(vmc);
        require(blend[0].value == 0 && blend[1].value == .7f && blend[2].value == .2f && blend[3].value == 0 &&
                blend[4].value == 1 && blend[5].value == 0 && blend[6].value == 0 &&
                blend[7].control == "Button_Menu" && blend[7].value == 1,
                "VMC exposes separate stick directions and controller button blends");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
