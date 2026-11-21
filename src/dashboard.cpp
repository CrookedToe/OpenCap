#include "bridge.hpp"
#include "desktop_preferences.hpp"

#include <windows.h>
#include <openvr.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace opencap;
constexpr int width = 900, height = 610;
constexpr char key[] = "opencap.dashboard";
constexpr COLORREF background = RGB(17, 27, 40);
constexpr COLORREF foreground = RGB(234, 242, 248);
constexpr COLORREF muted = RGB(151, 173, 187);
constexpr COLORREF accent = RGB(96, 224, 201);
constexpr COLORREF panel = RGB(31, 48, 65);

bool openDesktop(bool managed) {
    if (FindWindowW(L"OpenCapDesktop", nullptr)) return true;
    const auto directory = executableFolder();
    const auto executable = directory / L"OpenCap.exe";
    auto command = L"\"" + executable.wstring() + L"\"";
    if (managed) command += L" --steamvr-session " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    start.dwFlags = STARTF_USESHOWWINDOW;
    start.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       directory.c_str(), &start, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
    return false;
}

class Runtime {
public:
    explicit Runtime(vr::EVRApplicationType type) {
        vr::EVRInitError error = vr::VRInitError_None;
        vr::VR_Init(&error, type);
        if (error != vr::VRInitError_None)
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
    }
    ~Runtime() { vr::VR_Shutdown(); }
};

class Canvas {
public:
    Canvas() {
        dc_ = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        image_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        pixels_ = static_cast<std::uint32_t*>(pixels);
        if (dc_ && image_) previous_ = SelectObject(dc_, image_);
        title_ = font(39, FW_SEMIBOLD);
        body_ = font(25, FW_NORMAL);
        if (!pixels_ || !title_ || !body_) {
            cleanup();
            throw std::runtime_error("Could not allocate the dashboard surface.");
        }
        SetBkMode(dc_, TRANSPARENT);
    }

    ~Canvas() { cleanup(); }

    void rect(RECT area, COLORREF color) {
        auto fill = CreateSolidBrush(color);
        FillRect(dc_, &area, fill);
        DeleteObject(fill);
    }

    void text(int x, int y, std::wstring_view value, COLORREF color = foreground, bool title = false) {
        const auto previous = SelectObject(dc_, title ? title_ : body_);
        SetTextColor(dc_, color);
        TextOutW(dc_, x, y, value.data(), static_cast<int>(value.size()));
        SelectObject(dc_, previous);
    }

    void status(RECT area, std::wstring_view value) {
        const auto previous = SelectObject(dc_, body_);
        SetTextColor(dc_, muted);
        DrawTextW(dc_, value.data(), static_cast<int>(value.size()), &area,
                  DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc_, previous);
    }

    void label(RECT area, std::wstring_view value, bool enabled, bool selected = false) {
        rect(area, selected ? RGB(11, 94, 98) : panel);
        text(area.left + 12, area.top + 9, value, enabled ? foreground : muted);
    }

    bool upload(vr::IVROverlay& overlay, vr::VROverlayHandle_t handle) {
        GdiFlush();
        for (int i = 0; i < width * height; ++i) {
            const auto pixel = pixels_[i];
            pixels_[i] = 0xff000000u | (pixel & 0xff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
        }
        return overlay.SetOverlayRaw(handle, pixels_, width, height, 4) == vr::VROverlayError_None;
    }

private:
    static HFONT font(int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    }

    void cleanup() {
        if (previous_) SelectObject(dc_, previous_);
        if (image_) DeleteObject(image_);
        if (title_) DeleteObject(title_);
        if (body_) DeleteObject(body_);
        if (dc_) DeleteDC(dc_);
    }

    HDC dc_{};
    HBITMAP image_{};
    HGDIOBJ previous_{};
    HFONT title_{}, body_{};
    std::uint32_t* pixels_{};
};

constexpr RECT leftButton{36, 100, 268, 151};
constexpr RECT rightButton{282, 100, 514, 151};
constexpr RECT pageButton{650, 24, 862, 75};
constexpr RECT launchDesktop{36, 245, 310, 296};
constexpr RECT pairSelected{36, 244, 289, 295};
constexpr RECT pairBoth{304, 244, 561, 295};
constexpr RECT radioDown{36, 306, 155, 357};
constexpr RECT radioUp{170, 306, 289, 357};
constexpr RECT radioApply{304, 306, 561, 357};
constexpr RECT centerJoystick{36, 374, 289, 425};
constexpr RECT finishJoystick{304, 374, 561, 425};
constexpr RECT publishHand{36, 442, 289, 493};
constexpr RECT hapticsEnabled{304, 442, 561, 493};
constexpr RECT nextTracker{36, 510, 289, 561};
constexpr RECT autoTracker{304, 510, 561, 561};
constexpr RECT calibrate{36, 257, 324, 308};
constexpr RECT cancel{340, 257, 500, 308};
constexpr RECT quickCalibration{516, 257, 797, 308};
constexpr RECT joystick{36, 327, 324, 378};
constexpr RECT vibration{340, 327, 592, 378};
constexpr RECT alignMinus{36, 447, 168, 498};
constexpr RECT alignPlus{179, 447, 311, 498};
constexpr RECT alignAxis{326, 447, 539, 498};
constexpr RECT alignMode{550, 447, 862, 498};
constexpr RECT presetType{36, 521, 348, 572};
constexpr RECT applyPreset{366, 521, 590, 572};
constexpr std::array calibrationLabels{L"Calibrate hand", L"Capture fist", L"Capture fingers together", L"Capture fingers spread"};
constexpr std::array calibrationActions{Action::startHand, Action::fist, Action::together, Action::spread};

class Dashboard {
public:
    Dashboard() : runtime_(vr::VRApplication_Overlay), overlay_(vr::VROverlay()) {
        if (!overlay_) throw std::runtime_error("SteamVR did not provide the overlay interface.");
        const auto error = overlay_->CreateDashboardOverlay(key, "OpenCap", &main_, &thumbnail_);
        if (error != vr::VROverlayError_None)
            throw std::runtime_error(overlay_->GetOverlayErrorNameFromEnum(error));
        const vr::HmdVector2_t mouse{{static_cast<float>(width), static_cast<float>(height)}};
        overlay_->SetOverlayMouseScale(main_, &mouse);
        overlay_->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
        overlay_->SetOverlayWidthInMeters(main_, 1.45f);
        std::array<std::uint32_t, 64*64> icon{};
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto r = (x - 31)*(x - 31) + (y - 31)*(y - 31);
                icon[y*64+x] = r > 285 && r < 510 ? 0xffc9e060u : 0xff281b11u;
            }
        }
        overlay_->SetOverlayRaw(thumbnail_, icon.data(), 64, 64, 4);
    }

    ~Dashboard() {
        if (thumbnail_) overlay_->DestroyOverlay(thumbnail_);
        if (main_) overlay_->DestroyOverlay(main_);
    }

    int run() {
        if (!paint()) throw std::runtime_error("SteamVR rejected the dashboard texture.");
        if (loadSettings(configFolder(false)).steamLifecycle) openDesktop(true);
        auto lastPaint = now();
        while (true) {
            if (const auto update = bus_.read(1)) snapshot_ = *update;
            if (const auto update = bus_.readTracking()) tracking_ = *update;
            vr::VREvent_t event{};
            while (overlay_->PollNextOverlayEvent(main_, &event, sizeof(event))) {
                if (event.eventType == vr::VREvent_Quit) return 0;
                if (event.eventType == vr::VREvent_MouseButtonDown && event.data.mouse.button == vr::VRMouseButton_Left) {
                    click({static_cast<LONG>(event.data.mouse.x), height - static_cast<LONG>(event.data.mouse.y)});
                    lastPaint = 0;
                }
            }
            auto system = vr::VRSystem();
            while (system && system->PollNextEvent(&event, sizeof(event))) {
                if (event.eventType == vr::VREvent_Quit) {
                    system->AcknowledgeQuit_Exiting();
                    return 0;
                }
            }
            const auto time = now();
            if (time - lastPaint >= 100 && overlay_->IsOverlayVisible(main_)) {
                paint();
                lastPaint = time;
            }
            Sleep(15);
        }
    }

private:
    void send(Action action, int value = 0, float duration = 0, float amplitude = 0) {
        problem_ = bus_.send({static_cast<Side>(side_), action, value, duration, amplitude})
            ? L"" : L"The desktop command queue is busy.";
    }

    bool desktopAvailable(Millis time) const {
        return snapshot_.heartbeat && time >= snapshot_.heartbeat && time - snapshot_.heartbeat < sampleTimeout;
    }

    int proposedChannel() const {
        const auto current = snapshot_.hands[side_].channel;
        return channels_[side_].value_or(current >= 0 && current <= 100 ? current : 20);
    }

    bool duplicateChannel() const {
        const auto& other = snapshot_.hands[1 - side_];
        return other.receiver && proposedChannel() == other.channel;
    }

    void clickSetup(POINT point, Millis time) {
        const auto& hand = snapshot_.hands[side_];
        const auto live = isLive(snapshot_, static_cast<Side>(side_), time);
        const auto stage = hand.calibrationStep;
        if (hand.receiver && PtInRect(&pairSelected, point)) send(hand.pairing ? Action::pairStop : Action::pairStart);
        else if (hand.receiver && PtInRect(&pairBoth, point))
            send(hand.pairing || snapshot_.hands[1 - side_].pairing ? Action::pairBothStop : Action::pairBoth);
        else if (hand.receiver && PtInRect(&radioDown, point)) channels_[side_] = std::max(0, proposedChannel() - 1);
        else if (hand.receiver && PtInRect(&radioUp, point)) channels_[side_] = std::min(100, proposedChannel() + 1);
        else if (hand.receiver && !duplicateChannel() && PtInRect(&radioApply, point))
            send(Action::channel, proposedChannel());
        else if (live && stage == 0 && PtInRect(&centerJoystick, point)) send(Action::joystickCenter);
        else if (live && stage == 4 && PtInRect(&finishJoystick, point)) send(Action::joystickFinish);
        else if (PtInRect(&publishHand, point)) send(Action::togglePublishing);
        else if (PtInRect(&hapticsEnabled, point)) send(Action::toggleHaptics);
        else if (trackingCurrent(tracking_, time) && tracking_.count && PtInRect(&nextTracker, point))
            send(Action::cycleTracker);
        else if (PtInRect(&autoTracker, point)) send(Action::autoTracker);
    }

    void click(POINT point) {
        problem_ = L"";
        if (PtInRect(&leftButton, point)) { side_ = 0; return; }
        if (PtInRect(&rightButton, point)) { side_ = 1; return; }
        if (PtInRect(&pageButton, point)) { setup_ = !setup_; return; }
        const auto time = now();
        if (!desktopAvailable(time)) {
            if (PtInRect(&launchDesktop, point) && !openDesktop(false)) problem_ = L"Could not open the OpenCap desktop.";
            return;
        }
        if (setup_) { clickSetup(point, time); return; }
        if (!isLive(snapshot_, static_cast<Side>(side_), time)) return;
        const auto& hand = snapshot_.hands[side_];
        if (PtInRect(&calibrate, point) && hand.calibrationStep < calibrationActions.size())
            send(calibrationActions[hand.calibrationStep]);
        else if (PtInRect(&cancel, point) && hand.calibrationStep >= 1 && hand.calibrationStep <= 3)
            send(Action::cancelHand);
        else if (PtInRect(&quickCalibration, point) && hand.calibrationStep == 0) send(Action::guidedCalibration);
        else if (PtInRect(&joystick, point)) send(Action::toggleStick);
        else if (PtInRect(&vibration, point)) send(Action::haptic, 0, .12f, .8f);
        else if (PtInRect(&alignAxis, point)) axis_ = (axis_ + 1) % 3;
        else if (PtInRect(&alignMode, point)) rotation_ = !rotation_;
        else if (PtInRect(&alignMinus, point)) send(Action::adjustPose, axis_ + (rotation_ ? 3 : 0), 0, -1);
        else if (PtInRect(&alignPlus, point)) send(Action::adjustPose, axis_ + (rotation_ ? 3 : 0), 0, 1);
        else if (PtInRect(&presetType, point)) preset_ = (preset_ + 1) % trackingPresets.size();
        else if (PtInRect(&applyPreset, point)) send(Action::applyPreset, static_cast<int>(preset_));
    }

    void paintSetup(const Hand& hand, const HandSettings& settings, bool live, Millis time) {
        const auto stage = hand.calibrationStep;
        const auto eitherPairing = hand.pairing || snapshot_.hands[1 - side_].pairing;
        const auto viewChannel = hand.channel >= 0 ? std::to_wstring(hand.channel) : L"—";
        canvas_.text(36, 209, std::format(L"Receiver channel  {}   Selected  {}", viewChannel, proposedChannel()), muted);
        canvas_.label(pairSelected, hand.pairing ? L"Stop pairing" : L"Pair selected glove", hand.receiver);
        canvas_.label(pairBoth, eitherPairing ? L"Stop both" : L"Pair both gloves", hand.receiver);
        canvas_.label(radioDown, L"− Channel", hand.receiver);
        canvas_.label(radioUp, L"+ Channel", hand.receiver);
        canvas_.label(radioApply, L"Apply channel", hand.receiver && !duplicateChannel());
        canvas_.label(centerJoystick, L"Set joystick center", live && stage == 0);
        canvas_.label(finishJoystick, L"Finish joystick range", live && stage == 4);
        canvas_.label(publishHand, settings.enabled ? L"Hide SteamVR hand" : L"Show SteamVR hand", true);
        canvas_.label(hapticsEnabled, settings.haptics ? L"Disable haptics" : L"Enable haptics", true);
        canvas_.label(nextTracker, L"Next tracker", trackingCurrent(tracking_, time) && tracking_.count);
        canvas_.label(autoTracker, L"Automatic tracker", true);
        const auto serial = settings.trackerSerial[0] ? widen(settings.trackerSerial.data())
            : settings.tracker == noTracker ? L"Automatic" : std::format(L"#{}", settings.tracker);
        canvas_.status({579, 520, 862, 553}, serial);
        if (duplicateChannel())
            canvas_.status({579, 317, 862, 350}, L"Used by the other glove");
    }

    bool paint() {
        canvas_.rect({0, 0, width, height}, background);
        canvas_.text(36, 24, L"OpenCap", foreground, true);
        canvas_.label(pageButton, setup_ ? L"Calibration" : L"Device setup", true);
        const auto time = now();
        const auto& hand = snapshot_.hands[side_];
        const auto& settings = snapshot_.settings[side_];
        const auto active = isLive(snapshot_, static_cast<Side>(side_), time);
        for (std::size_t i = 0; i < handCount; ++i) {
            const auto connected = isLive(snapshot_, static_cast<Side>(i), time);
            const auto label = std::format(L"{}  ·  {}", i ? L"Right" : L"Left", connected ? L"Live" : L"Offline");
            canvas_.label(i ? rightButton : leftButton, label, true, i == side_);
        }
        if (trackingCurrent(tracking_, time)) {
            const auto tracker = tracking_.selected[side_];
            canvas_.text(543, 109, tracker == noTracker ? L"No pose source" : std::format(L"Pose source  #{}", tracker), muted);
        }
        if (active) {
            const auto battery = hand.battery >= 0 ? std::format(L"{:.0f}%", 100 * hand.battery) : L"—";
            const auto signal = hand.signal ? std::format(L"{} dBm", hand.signal) : L"—";
            canvas_.text(36, 176, std::format(L"Battery {}   ·   {:.0f} fps   ·   {}{}", battery, hand.fps, signal,
                snapshot_.demo ? L"   ·   Simulated" : L""), accent);
        } else {
            canvas_.text(36, 176, time >= snapshot_.heartbeat && snapshot_.heartbeat && time - snapshot_.heartbeat < sampleTimeout
                ? L"The selected glove is offline." : L"Open the desktop app to connect your gloves.", muted);
        }
        if (!problem_.empty()) canvas_.status({36, 578, 862, 609}, problem_);
        else if (hand.message[0]) canvas_.status({36, 578, 862, 609}, widen(hand.message.data()));
        if (!desktopAvailable(time)) {
            canvas_.label(launchDesktop, L"Open desktop", true);
            return canvas_.upload(*overlay_, main_);
        }
        if (setup_) {
            paintSetup(hand, settings, active, time);
            return canvas_.upload(*overlay_, main_);
        }
        canvas_.text(36, 217, L"Glove controls", muted);
        const auto stage = hand.calibrationStep;
        canvas_.label(calibrate, stage < calibrationLabels.size() ? calibrationLabels[stage] : L"Joystick calibration",
                      active && stage < calibrationLabels.size());
        canvas_.label(cancel, L"Cancel", active && stage >= 1 && stage <= 3);
        canvas_.label(quickCalibration, L"Guided calibration", active && stage == 0);
        canvas_.label(joystick, settings.joystick ? L"Disable joystick" : L"Enable joystick", active);
        canvas_.label(vibration, L"Test vibration", active);
        const auto v = rotation_ ? settings.rotation : settings.position;
        const std::array coordinates{v.x, v.y, v.z};
        const auto name = std::wstring(1, L"XYZ"[axis_]);
        const auto value = rotation_ ? std::format(L"{:+.0f}°", coordinates[axis_])
            : std::format(L"{:+.0f} mm", coordinates[axis_]*1000);
        canvas_.text(36, 405, std::format(L"Tracker alignment   {}   {}", name, value), muted);
        canvas_.label(alignMinus, L"− Step", active);
        canvas_.label(alignPlus, L"+ Step", active);
        canvas_.label(alignAxis, L"Axis " + name, active);
        canvas_.label(alignMode, rotation_ ? L"Adjust position" : L"Adjust rotation", active);
        canvas_.label(presetType, trackingPresets[preset_].name, active);
        canvas_.label(applyPreset, L"Apply to both hands", active);
        return canvas_.upload(*overlay_, main_);
    }

    Runtime runtime_;
    vr::IVROverlay* overlay_{};
    vr::VROverlayHandle_t main_{}, thumbnail_{};
    Canvas canvas_;
    Bus bus_;
    Snapshot snapshot_{};
    TrackingSnapshot tracking_{};
    std::size_t side_{};
    std::size_t preset_{};
    int axis_{};
    bool rotation_{};
    bool setup_{};
    std::array<std::optional<int>, handCount> channels_{};
    std::wstring problem_;
};

int registration(bool remove) {
    Runtime runtime(vr::VRApplication_Background);
    const auto path = (executableFolder() / L"opencap.vrmanifest").u8string();
    const auto manifest = reinterpret_cast<const char*>(path.c_str());
    auto apps = vr::VRApplications();
    if (!apps) throw std::runtime_error("SteamVR applications interface is unavailable.");
    auto result = remove ? apps->RemoveApplicationManifest(manifest) : apps->AddApplicationManifest(manifest, false);
    if (!remove && result == vr::VRApplicationError_None)
        result = apps->SetApplicationAutoLaunch(key, true);
    if (result != vr::VRApplicationError_None)
        throw std::runtime_error(apps->GetApplicationsErrorNameFromEnum(result));
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR line, int) {
    const std::wstring_view args(line);
    try {
        if (args == L"--register") return registration(false);
        if (args == L"--unregister") return registration(true);
        Dashboard dashboard;
        return dashboard.run();
    } catch (const std::exception& error) {
        if (args == L"--register" || args == L"--unregister") OutputDebugStringA(error.what());
        else MessageBoxA(nullptr, error.what(), "OpenCap Dashboard", MB_OK | MB_ICONERROR);
        return 1;
    }
}
