#include "bridge.hpp"
#include "desktop_preferences.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openvr.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace opencap;
constexpr int width = 1120, height = 612;
constexpr char key[] = "opencap.dashboard";
constexpr COLORREF background = RGB(13, 21, 31);
constexpr COLORREF surface = RGB(24, 37, 52);
constexpr COLORREF border = RGB(44, 63, 80);
constexpr COLORREF foreground = RGB(244, 249, 251);
constexpr COLORREF muted = RGB(169, 187, 201);
constexpr COLORREF accent = RGB(103, 229, 204);
constexpr COLORREF accentInk = RGB(10, 35, 35);
constexpr COLORREF warning = RGB(255, 198, 126);
constexpr Millis commandWait = 1500;

constexpr RECT desktopButton{868, 35, 1080, 93};
constexpr std::array<RECT, handCount> gloveCards{{{40, 128, 548, 350}, {572, 128, 1080, 350}}};
constexpr RECT calibrationCard{40, 374, 1080, 574};
constexpr RECT bothButton{800, 416, 1056, 475};
constexpr RECT cancelButton{870, 397, 1056, 451};
constexpr std::array handNames{L"Left glove", L"Right glove"};

bool openDesktop(bool managed) {
    if (const auto window = FindWindowW(L"OpenCapDesktop", nullptr)) {
        if (!managed) {
            if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        return true;
    }
    const auto directory = executableFolder();
    const auto executable = directory / L"OpenCap.exe";
    auto command = L"\"" + executable.wstring() + L"\"";
    if (managed) command += L" --steamvr-session " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    start.dwFlags = STARTF_USESHOWWINDOW;
    start.wShowWindow = managed ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &start, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
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

enum class Font { title, heading, body, label };
enum class ButtonStyle { primary, secondary };

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
        fonts_ = {font(47, FW_SEMIBOLD), font(34, FW_SEMIBOLD), font(28, FW_NORMAL), font(23, FW_SEMIBOLD)};
        if (!dc_ || !previous_ || !pixels_ || std::ranges::find(fonts_, nullptr) != fonts_.end()) {
            cleanup();
            throw std::runtime_error("Could not allocate the dashboard surface.");
        }
        SetBkMode(dc_, TRANSPARENT);
    }
    ~Canvas() { cleanup(); }

    void rect(RECT area, COLORREF color) {
        SetDCBrushColor(dc_, color);
        FillRect(dc_, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void rounded(RECT area, int radius, COLORREF color) {
        const auto lastPen = SelectObject(dc_, GetStockObject(NULL_PEN));
        const auto lastBrush = SelectObject(dc_, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc_, color);
        RoundRect(dc_, area.left, area.top, area.right, area.bottom, radius, radius);
        SelectObject(dc_, lastBrush);
        SelectObject(dc_, lastPen);
    }

    void text(RECT area, std::wstring_view value, Font style, COLORREF color = foreground, UINT align = DT_LEFT) {
        const auto previous = SelectObject(dc_, fonts_[static_cast<std::size_t>(style)]);
        SetTextColor(dc_, color);
        DrawTextW(dc_, value.data(), static_cast<int>(value.size()), &area,
                  align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc_, previous);
    }

    void button(RECT area, std::wstring_view value, bool enabled, ButtonStyle style, POINT cursor) {
        const bool hover = enabled && PtInRect(&area, cursor);
        const bool primary = enabled && style == ButtonStyle::primary;
        const auto fill = primary ? (hover ? RGB(145, 246, 223) : accent)
            : hover ? RGB(54, 78, 96) : enabled ? border : RGB(31, 46, 61);
        rounded(area, 16, fill);
        InflateRect(&area, -10, 0);
        text(area, value, Font::body, primary ? accentInk : enabled ? foreground : muted, DT_CENTER);
    }

    std::span<const std::uint32_t> frame() {
        GdiFlush();
        for (int i = 0; i < width * height; ++i) {
            const auto pixel = pixels_[i];
            pixels_[i] = 0xff000000u | (pixel & 0xff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
        }
        return {pixels_, width * height};
    }

private:
    static HFONT font(int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    }

    void cleanup() {
        if (previous_) SelectObject(dc_, previous_);
        if (image_) DeleteObject(image_);
        for (auto font : fonts_) if (font) DeleteObject(font);
        if (dc_) DeleteDC(dc_);
    }

    HDC dc_{};
    HBITMAP image_{};
    HGDIOBJ previous_{};
    std::array<HFONT, 4> fonts_{};
    std::uint32_t* pixels_{};
};

class OverlayTextures {
public:
    explicit OverlayTextures(vr::IVRSystem* system) {
        if (!system) throw std::runtime_error("SteamVR did not provide the system interface.");
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
            throw std::runtime_error("Could not initialize DXGI for the dashboard.");
        int32_t index = -1;
        system->GetDXGIOutputInfo(&index);
        ComPtr<IDXGIAdapter1> adapter;
        if (index >= 0 && FAILED(factory->EnumAdapters1(static_cast<UINT>(index), adapter.GetAddressOf())))
            throw std::runtime_error("Could not find SteamVR's graphics adapter.");
        const auto type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        if (FAILED(D3D11CreateDevice(adapter.Get(), type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    device_.GetAddressOf(), nullptr, context_.GetAddressOf())))
            throw std::runtime_error("Could not create a Direct3D device for the dashboard.");
        main_ = create(width, height);
        thumbnail_ = create(64, 64);
    }

    void setIcon(vr::IVROverlay& overlay, vr::VROverlayHandle_t handle) {
        std::array<std::uint32_t, 64*64> pixels{};
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto radiusSquared = (x - 31)*(x - 31) + (y - 31)*(y - 31);
                pixels[y*64+x] = radiusSquared >= 210 && radiusSquared <= 480 ? 0xffcce567u : 0xff1f150du;
            }
        }
        const auto result = submit(overlay, handle, *thumbnail_.Get(), pixels.data(), 64);
        if (result != vr::VROverlayError_None) throw std::runtime_error(overlay.GetOverlayErrorNameFromEnum(result));
    }

    vr::EVROverlayError present(vr::IVROverlay& overlay, vr::VROverlayHandle_t handle, std::span<const std::uint32_t> pixels) {
        if (std::ranges::equal(lastFrame_, pixels)) return vr::VROverlayError_None;
        const auto result = submit(overlay, handle, *main_.Get(), pixels.data(), width);
        if (result == vr::VROverlayError_None) lastFrame_.assign(pixels.begin(), pixels.end());
        return result;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> create(UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = w;
        description.Height = h;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device_->CreateTexture2D(&description, nullptr, texture.GetAddressOf())))
            throw std::runtime_error("Could not create a SteamVR overlay texture.");
        return texture;
    }

    vr::EVROverlayError submit(vr::IVROverlay& overlay, vr::VROverlayHandle_t handle, ID3D11Texture2D& resource,
                               const std::uint32_t* pixels, UINT rowWidth) {
        context_->UpdateSubresource(&resource, 0, nullptr, pixels, rowWidth * sizeof(*pixels), 0);
        context_->Flush();
        const vr::Texture_t texture{&resource, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        return overlay.SetOverlayTexture(handle, &texture);
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> main_, thumbnail_;
    std::vector<std::uint32_t> lastFrame_;
};

bool desktopAvailable(const Snapshot& snapshot, Millis time) {
    return snapshot.heartbeat && time >= snapshot.heartbeat && time - snapshot.heartbeat <= sampleTimeout;
}

struct DashboardState {
    const Snapshot& snapshot;
    const TrackingSnapshot& tracking;
    Millis time;
    bool starting;
    std::wstring_view notice;

    bool desktop() const { return desktopAvailable(snapshot, time); }
    bool live(std::size_t hand) const { return isLive(snapshot, static_cast<Side>(hand), time); }
    bool guiding() const { return desktop() && snapshot.calibrationGuide.hands; }
    bool canCalibrateBoth() const { return !starting && handCalibrationReady(snapshot, time); }
};

enum class DashboardAction { none, desktop, calibrate, cancel };

DashboardAction actionAt(const DashboardState& state, POINT point) {
    if (PtInRect(&desktopButton, point)) return DashboardAction::desktop;
    if (state.guiding() && PtInRect(&cancelButton, point)) return DashboardAction::cancel;
    if (state.canCalibrateBoth() && PtInRect(&bothButton, point)) return DashboardAction::calibrate;
    return DashboardAction::none;
}

void drawGlove(Canvas& canvas, const DashboardState& state, std::size_t index) {
    const auto card = gloveCards[index];
    const auto x = card.left + 24;
    const auto right = card.right - 24;
    const auto& hand = state.snapshot.hands[index];
    const auto& settings = state.snapshot.settings[index];
    const bool live = state.live(index);
    const std::wstring_view linkLabel = live ? L"Connected" : state.desktop() && hand.receiver ? L"Glove offline" : L"No receiver";
    const auto linkColor = live ? accent : muted;
    canvas.rounded(card, 24, surface);
    canvas.text({x, 147, x + 250, 194}, handNames[index], Font::heading);
    canvas.rounded({right - 152, 153, right, 190}, 30, live ? RGB(19, 66, 62) : border);
    canvas.text({right - 150, 153, right - 2, 190}, linkLabel, Font::label, linkColor, DT_CENTER);
    canvas.rect({x, 202, right, 204}, border);

    const auto middle = x + 228;
    const auto battery = live && hand.battery >= 0 ? std::format(L"{:.0f}%", hand.battery * 100) : L"—";
    const auto signal = live && hand.signal < 0 ? radioSignalLabel(hand, true) : L"—";
    canvas.text({x, 216, middle, 244}, L"Battery", Font::label, muted);
    canvas.text({x, 247, middle, 286}, battery, Font::heading);
    canvas.text({middle + 16, 216, right, 244}, L"Radio signal", Font::label, muted);
    canvas.text({middle + 16, 247, right, 286}, signal, Font::body);

    const auto tracked = live && settings.enabled && trackingCurrent(state.tracking, state.time)
        && state.tracking.selected[index] != noTracker;
    const auto tracker = tracked ? L"Tracker locked" : live && !settings.enabled ? L"Tracking disabled"
        : live && trackingCurrent(state.tracking, state.time) ? L"Waiting for tracker" : L"Tracking offline";
    const auto calibration = state.guiding() && (state.snapshot.calibrationGuide.hands & (1u << index)) ? L"Calibrating"
        : live && hand.calibrationStep == 4 ? L"Joystick setup active"
        : live && hand.calibrated ? L"Calibrated" : live ? L"Calibration needed" : L"";
    canvas.text({x, 295, middle + 8, 325}, tracker, Font::label, tracked ? accent : muted);
    canvas.text({middle + 16, 295, right, 325}, calibration, Font::label, live && !hand.calibrated ? warning : muted);
}

void drawCalibration(Canvas& canvas, const DashboardState& state, POINT cursor) {
    canvas.rounded(calibrationCard, 24, surface);
    const auto& guide = state.snapshot.calibrationGuide;
    if (!state.guiding()) {
        canvas.text({64, 399, 766, 445}, L"Hand calibration", Font::heading);
        auto message = state.notice;
        if (message.empty()) {
            if (state.starting) message = L"Starting calibration…";
            else if (!state.desktop()) message = L"Open the desktop app to connect your gloves.";
            else if (guide.message[0]) message = guide.message.data();
            else if (!state.live(0) || !state.live(1)) message = L"Connect both gloves to start calibration.";
            else if (!state.canCalibrateBoth()) message = L"Finish joystick calibration in the desktop app first.";
            else message = L"Fist  ·  Fingers together  ·  Fingers spread";
        }
        const auto color = !state.notice.empty() || (state.desktop() && guide.result == CalibrationResult::failed && !state.starting)
            ? warning : state.desktop() && guide.result == CalibrationResult::saved && !state.starting ? accent : muted;
        canvas.text({64, 485, 1056, 545}, message, Font::body, color);
        canvas.button(bothButton, L"Calibrate gloves", state.canCalibrateBoth(), ButtonStyle::primary, cursor);
        return;
    }

    canvas.text({64, 397, 850, 451}, L"Calibrating both gloves", Font::heading);
    canvas.button(cancelButton, L"Cancel", true, ButtonStyle::secondary, cursor);

    const auto remaining = guide.captureAt > state.time ? (std::min)(guide.captureAt - state.time, calibrationCountdown) : 0;
    std::wstring prompt;
    if (remaining) prompt = std::format(L"{} s  ·  {}", (remaining + 999) / 1000, calibrationPose(guide.phase));
    else if (guide.phase > 3) prompt = L"Saving calibration…";
    else if (guide.phase == 1 && !guide.captureAt) prompt = L"Getting ready…";
    else prompt = L"Recording pose…";
    canvas.text({64, 455, 1056, 495}, prompt, Font::body, accent);

    constexpr std::array stepLabels{L"Fist", L"Fingers together", L"Fingers spread"};
    for (unsigned step = 0; step < stepLabels.size(); ++step) {
        const LONG left = 64 + 334L * step;
        const auto phase = step + 1;
        const bool current = phase == guide.phase;
        const bool captured = phase < guide.phase;
        const RECT track{left, 510, left + 324, 518};
        canvas.rounded(track, 8, border);
        auto fill = track;
        if (current && guide.captureAt) fill.right -= static_cast<LONG>(324 * remaining / calibrationCountdown);
        if (captured || (current && guide.captureAt && fill.right > fill.left)) canvas.rounded(fill, 8, accent);
        canvas.text({left, 526, track.right, 555}, stepLabels[step], Font::label, current || captured ? accent : muted);
    }
    if (!state.notice.empty())
        canvas.text({570, 397, 850, 451}, state.notice, Font::label, warning, DT_RIGHT);
}

void drawDashboard(Canvas& canvas, const DashboardState& state, POINT cursor) {
    canvas.rect({0, 0, width, height}, background);
    canvas.rounded({40, 37, 98, 95}, 18, accent);
    canvas.text({40, 37, 98, 95}, L"O", Font::heading, accentInk, DT_CENTER);
    canvas.text({115, 30, 755, 100}, L"OpenCap", Font::title);
    canvas.button(desktopButton, L"Open desktop", true, ButtonStyle::secondary, cursor);
    for (std::size_t index = 0; index < handCount; ++index) drawGlove(canvas, state, index);
    drawCalibration(canvas, state, cursor);
}

class Dashboard {
public:
    Dashboard() : overlay_(vr::VROverlay()), textures_(vr::VRSystem()) {
        if (!overlay_) throw std::runtime_error("SteamVR did not provide the overlay interface.");
        const auto error = overlay_->CreateDashboardOverlay(key, "OpenCap", &main_, &thumbnail_);
        if (error != vr::VROverlayError_None)
            throw std::runtime_error(overlay_->GetOverlayErrorNameFromEnum(error));
        const vr::HmdVector2_t mouse{{static_cast<float>(width), static_cast<float>(height)}};
        overlay_->SetOverlayMouseScale(main_, &mouse);
        overlay_->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
        overlay_->SetOverlayWidthInMeters(main_, 1.45f);
    }

    ~Dashboard() {
        if (thumbnail_) overlay_->DestroyOverlay(thumbnail_);
        if (main_) overlay_->DestroyOverlay(main_);
    }

    int run(bool show) {
        textures_.setIcon(*overlay_, thumbnail_);
        if (const auto result = paint(now()); result != vr::VROverlayError_None)
            throw std::runtime_error(std::string("Could not submit the dashboard texture: ") + overlay_->GetOverlayErrorNameFromEnum(result));
        if (show) overlay_->ShowDashboard(key);
        const auto folder = configFolder(false);
        const auto settings = loadSettings(folder);
        launchUpdater(true, settings.includePrereleases);
        if (steamLifecycleEnabled(folder, settings)) openDesktop(true);
        auto lastPaint = now();
        while (true) {
            if (const auto update = bus_.read(1)) {
                if (startingUntil_ && (update->calibrationGuide.hands ||
                    update->calibrationGuide.result != snapshot_.calibrationGuide.result ||
                    update->calibrationGuide.message != snapshot_.calibrationGuide.message)) startingUntil_ = 0;
                snapshot_ = *update;
            }
            const auto time = now();
            if (startingUntil_ && time >= startingUntil_) {
                startingUntil_ = 0;
                notice_ = L"Calibration didn’t start. Check the glove connection and try again.";
            }

            vr::VREvent_t event{};
            while (overlay_->PollNextOverlayEvent(main_, &event, sizeof(event))) {
                if (event.eventType == vr::VREvent_Quit) return 0;
                if (event.eventType == vr::VREvent_MouseMove) {
                    const auto point = pointer(event);
                    const auto current = state(time);
                    if (actionAt(current, point) != actionAt(current, cursor_)) lastPaint = 0;
                    cursor_ = point;
                } else if (event.eventType == vr::VREvent_FocusLeave || event.eventType == vr::VREvent_OverlayHidden) {
                    cursor_ = {-1, -1};
                    lastPaint = 0;
                } else if (event.eventType == vr::VREvent_MouseButtonDown && event.data.mouse.button == vr::VRMouseButton_Left) {
                    cursor_ = pointer(event);
                    click(cursor_, time);
                    lastPaint = 0;
                }
            }
            if (auto* system = vr::VRSystem()) {
                while (system->PollNextEvent(&event, sizeof(event))) {
                    if (event.eventType == vr::VREvent_Quit) {
                        system->AcknowledgeQuit_Exiting();
                        return 0;
                    }
                }
            }
            if (overlay_->IsOverlayVisible(main_) && (!lastPaint || time - lastPaint >= 100)) {
                if (const auto update = bus_.readTracking()) tracking_ = *update;
                const auto result = paint(time);
                if (result != lastTextureError_ && result != vr::VROverlayError_None) {
                    const auto message = std::string("OpenCap dashboard texture: ") + overlay_->GetOverlayErrorNameFromEnum(result) + '\n';
                    OutputDebugStringA(message.c_str());
                }
                lastTextureError_ = result;
                lastPaint = time;
            }
            Sleep(12);
        }
    }

private:
    static POINT pointer(const vr::VREvent_t& event) {
        return {static_cast<LONG>(event.data.mouse.x), height - 1 - static_cast<LONG>(event.data.mouse.y)};
    }

    DashboardState state(Millis time) const {
        return {snapshot_, tracking_, time, startingUntil_ != 0, notice_};
    }

    void click(POINT point, Millis time) {
        switch (actionAt(state(time), point)) {
        case DashboardAction::desktop:
            notice_ = openDesktop(false) ? L"" : L"Could not open the OpenCap desktop.";
            break;
        case DashboardAction::cancel:
            notice_ = bus_.send({Side::left, Action::cancelHand}) ? L"" : L"Try canceling again.";
            break;
        case DashboardAction::calibrate: startCalibration(time); break;
        case DashboardAction::none: break;
        }
    }

    void startCalibration(Millis time) {
        if (bus_.send({Side::left, Action::guidedCalibration})) {
            notice_.clear();
            startingUntil_ = time + commandWait;
        } else notice_ = L"Could not start calibration. Try again.";
    }

    vr::EVROverlayError paint(Millis time) {
        drawDashboard(canvas_, state(time), cursor_);
        return textures_.present(*overlay_, main_, canvas_.frame());
    }

    vr::IVROverlay* overlay_{};
    vr::VROverlayHandle_t main_{}, thumbnail_{};
    OverlayTextures textures_;
    vr::EVROverlayError lastTextureError_{vr::VROverlayError_None};
    Canvas canvas_;
    Bus bus_;
    Snapshot snapshot_{};
    TrackingSnapshot tracking_{};
    POINT cursor_{-1, -1};
    Millis startingUntil_{};
    std::wstring notice_;
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
        Runtime runtime(vr::VRApplication_Overlay);
        const bool show = args == L"--show";
        if (show) {
            auto* overlay = vr::VROverlay();
            vr::VROverlayHandle_t existing{};
            if (overlay && overlay->FindOverlay(key, &existing) == vr::VROverlayError_None) {
                overlay->ShowDashboard(key);
                return 0;
            }
        }
        Dashboard dashboard;
        return dashboard.run(show);
    } catch (const std::exception& error) {
        if (args == L"--register" || args == L"--unregister") OutputDebugStringA(error.what());
        else MessageBoxA(nullptr, error.what(), "OpenCap Dashboard", MB_OK | MB_ICONERROR);
        return 1;
    }
}
