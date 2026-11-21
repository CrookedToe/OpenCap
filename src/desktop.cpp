#include "bridge.hpp"
#include "stream.hpp"
#ifdef OPENCAP_RECEIVER
#include "receiver.hpp"
#endif

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace {
using namespace opencap;

constexpr COLORREF background = RGB(248, 250, 252);
constexpr COLORREF ink = RGB(22, 36, 48);
constexpr COLORREF accent = RGB(0, 111, 113);
constexpr UINT_PTR tickTimer = 1;
constexpr int winWidth = 920, winHeight = 765;
enum Control {
    leftHand = 100, rightHand, nextPose, cancelPose, centerStick, finishStick, pair,
    stopPair, pairBoth, channelValue, setChannel, vibrate, useHand, useJoystick, useHaptics,
    trackerNum, posX, posY, posZ, rotX, rotY, rotZ, applyPose, udpIp, vmcPort, oscPort,
    applyStream, saveCsv, dashboard, globalHotkey
};

struct Preferences {
    std::array<HandSettings, handCount> hands{};
    StreamSettings stream{};
    bool hotkey{};
};

std::filesystem::path executableFolder() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path configFolder() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) return {};
    std::filesystem::path folder(path);
    CoTaskMemFree(path);
    folder /= L"OpenCap";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path{} : folder;
}

bool validPose(const HandSettings& hand) {
    const auto p = hand.position;
    const auto r = hand.rotation;
    return (hand.tracker == noTracker || (hand.tracker >= 1 && hand.tracker < 64))
        && std::abs(p.x) <= 1 && std::abs(p.y) <= 1 && std::abs(p.z) <= 1
        && std::abs(r.x) <= 360 && std::abs(r.y) <= 360 && std::abs(r.z) <= 360;
}

Preferences loadSettings(const std::filesystem::path& path) {
    Preferences settings;
    if (path.empty()) return settings;
    std::ifstream input(path / "settings.txt");
    input.imbue(std::locale::classic());
    int version = 0;
    if (!(input >> version) || version != 1) return {};
    for (auto& hand : settings.hands) {
        auto& p = hand.position;
        auto& r = hand.rotation;
        if (!(input >> hand.tracker >> p.x >> p.y >> p.z >> r.x >> r.y >> r.z
                    >> hand.enabled >> hand.joystick >> hand.haptics) || !validPose(hand)) return {};
    }
    auto& s = settings.stream;
    if (!(input >> std::quoted(s.address) >> s.vmcPort >> s.oscPort >> settings.hotkey)
        || s.vmcPort > 65535 || s.oscPort > 65535) return {};
    return settings;
}

bool saveSettings(const std::filesystem::path& path, const Preferences& settings) {
    if (path.empty()) return false;
    const auto tmp = path / "settings.tmp";
    {
        std::ofstream output(tmp, std::ios::trunc);
        output.imbue(std::locale::classic());
        output << "1\n" << std::setprecision(8);
        for (const auto& hand : settings.hands) {
            const auto p = hand.position, r = hand.rotation;
            output << hand.tracker << ' ' << p.x << ' ' << p.y << ' ' << p.z << ' '
                   << r.x << ' ' << r.y << ' ' << r.z << ' '
                   << hand.enabled << ' ' << hand.joystick << ' ' << hand.haptics << '\n';
        }
        const auto& s = settings.stream;
        output << std::quoted(s.address) << ' ' << s.vmcPort << ' ' << s.oscPort << ' ' << settings.hotkey << '\n';
        output.close();
        if (!output) return false;
    }
    return MoveFileExW(tmp.c_str(), (path / "settings.txt").c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

constexpr std::array<const wchar_t*, handCount> sideNames{L"Left glove", L"Right glove"};

struct CalibrationControl {
    const wchar_t* label;
    Action action;
};
constexpr std::array calibration{
    CalibrationControl{L"Start hand calibration", Action::startHand},
    CalibrationControl{L"Capture fist", Action::fist},
    CalibrationControl{L"Capture fingers together", Action::together},
    CalibrationControl{L"Capture fingers spread", Action::spread},
    CalibrationControl{L"Finish joystick range first", Action::startHand}
};

class Desktop {
public:
    explicit Desktop(bool demo) : demo_(demo), folder_(configFolder()), prefs_(loadSettings(folder_)) {
        snapshot_.demo = demo_;
        snapshot_.settings = prefs_.hands;
        if (!stream_.configure(prefs_.stream)) {
            prefs_.stream = {};
            stream_.configure(prefs_.stream);
        }
#ifdef OPENCAP_RECEIVER
        snapshot_.receiverSupported = true;
        if (!demo_ && !folder_.empty()) receiver_ = std::make_unique<Receiver>(folder_ / "Gloves");
#endif
    }

    ~Desktop() {
        snapshot_.heartbeat = 0;
        snapshot_.hands = {};
        bus_.publish(snapshot_);
        if (font_) DeleteObject(font_);
        if (title_) DeleteObject(title_);
        if (brush_) DeleteObject(brush_);
    }

    int run(HINSTANCE instance, int view) {
        WNDCLASSW cls{};
        cls.hInstance = instance;
        cls.lpszClassName = L"OpenCapDesktop";
        cls.lpfnWndProc = dispatch;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&cls)) return 1;
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT area{0, 0, winWidth, winHeight};
        AdjustWindowRect(&area, style, FALSE);
        window_ = CreateWindowW(cls.lpszClassName, L"OpenCap", style, CW_USEDEFAULT, CW_USEDEFAULT,
            area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        ShowWindow(window_, view);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) send(Action::startHand);
            else if (!IsDialogMessageW(window_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return static_cast<int>(msg.wParam);
    }

private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto self = static_cast<Desktop*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        const auto self = reinterpret_cast<Desktop*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->event(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT event(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: create(); return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) click(LOWORD(wParam));
            return 0;
        case WM_TIMER: tick(); return 0;
        case WM_HOTKEY: if (wParam == 1) send(Action::startHand); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_DESTROY:
            UnregisterHotKey(window_, 1);
            KillTimer(window_, tickTimer);
            stream_.configure({});
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND item(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        const auto control = CreateWindowExW(style & WS_BORDER ? WS_EX_CLIENTEDGE : 0, type, text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (style & ~WS_BORDER), x, y, w, h, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
    }

    void button(int id, const wchar_t* caption, int x, int y, int w, DWORD extra = BS_PUSHBUTTON) {
        item(L"BUTTON", caption, extra, x, y, w, 29, id);
    }

    void edit(int id, int x, int y, int w) {
        item(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x, y, w, 26, id);
    }

    void create() {
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        title_ = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        brush_ = CreateSolidBrush(background);
        button(leftHand, L"Left glove", 30, 161, 130, BS_AUTORADIOBUTTON | WS_GROUP);
        button(rightHand, L"Right glove", 170, 161, 130, BS_AUTORADIOBUTTON);
        button(dashboard, L"Open in SteamVR", 707, 27, 182);
        button(nextPose, calibration[0].label, 422, 235, 228, WS_GROUP | BS_PUSHBUTTON);
        button(cancelPose, L"Cancel", 662, 235, 103);
        button(centerStick, L"Set joystick center", 422, 272, 228);
        button(finishStick, L"Finish joystick range", 662, 272, 227);
        button(pair, L"Pair glove", 422, 321, 140);
        button(stopPair, L"Stop pairing", 572, 321, 138);
        button(pairBoth, L"Pair both", 720, 321, 169);
        edit(channelValue, 521, 359, 70);
        button(setChannel, L"Set channel", 602, 357, 128);
        button(vibrate, L"Test vibration", 741, 357, 148);
        button(useHand, L"Publish to SteamVR", 422, 400, 172, BS_AUTOCHECKBOX);
        button(useJoystick, L"Joystick", 600, 400, 110, BS_AUTOCHECKBOX);
        button(useHaptics, L"Vibration", 729, 400, 133, BS_AUTOCHECKBOX);
        edit(trackerNum, 422, 497, 54);
        edit(posX, 484, 497, 58); edit(posY, 550, 497, 58); edit(posZ, 616, 497, 58);
        edit(rotX, 682, 497, 58); edit(rotY, 748, 497, 58); edit(rotZ, 814, 497, 58);
        button(applyPose, L"Apply tracker alignment", 422, 531, 228);
        button(saveCsv, L"Export hand sample", 30, 528, 181);
        button(globalHotkey, L"F1 quick calibration", 662, 531, 227, BS_AUTOCHECKBOX);
        edit(udpIp, 30, 654, 183);
        edit(vmcPort, 226, 654, 93);
        edit(oscPort, 332, 654, 93);
        button(applyStream, L"Apply streaming", 438, 651, 167);
        SetDlgItemTextW(window_, udpIp, widen(prefs_.stream.address).c_str());
        SetDlgItemInt(window_, vmcPort, prefs_.stream.vmcPort, FALSE);
        SetDlgItemInt(window_, oscPort, prefs_.stream.oscPort, FALSE);
        if (prefs_.hotkey && !RegisterHotKey(window_, 1, MOD_NOREPEAT, VK_F1)) prefs_.hotkey = false;
        CheckDlgButton(window_, globalHotkey, prefs_.hotkey ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(window_, channelValue, 20, FALSE);
        select(0);
        SetTimer(window_, tickTimer, 16, nullptr);
    }

    void select(std::size_t side) {
        side_ = side;
        CheckRadioButton(window_, leftHand, rightHand, side ? rightHand : leftHand);
        const auto& h = prefs_.hands[side];
        CheckDlgButton(window_, useHand, h.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, useJoystick, h.joystick ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, useHaptics, h.haptics ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(window_, trackerNum, h.tracker == noTracker ? L"Auto" : std::to_wstring(h.tracker).c_str());
        for (const auto [id, value] : std::array{
            std::pair{posX, h.position.x}, std::pair{posY, h.position.y}, std::pair{posZ, h.position.z},
            std::pair{rotX, h.rotation.x}, std::pair{rotY, h.rotation.y}, std::pair{rotZ, h.rotation.z}}) {
            SetDlgItemTextW(window_, id, std::format(L"{:.3f}", value).c_str());
        }
        lastStage_ = UINT32_MAX;
        status_.clear();
    }

    std::wstring field(int id) const {
        wchar_t value[128]{};
        GetDlgItemTextW(window_, id, value, 128);
        return value;
    }

    template <class Number>
    static Number parse(const std::wstring& field) {
        std::wistringstream text(field);
        text.imbue(std::locale::classic());
        Number value{};
        if (!(text >> value) || (text >> std::ws && !text.eof())) throw std::invalid_argument("Invalid field");
        return value;
    }

    void save() {
        status_ = saveSettings(folder_, prefs_) ? L"Settings saved." : L"Could not save settings.";
    }

    void click(int id) {
        auto& settings = prefs_.hands[side_];
        const auto& hand = snapshot_.hands[side_];
        switch (id) {
        case leftHand: select(0); break;
        case rightHand: select(1); break;
        case nextPose:
            if (hand.calibrationStep < 4) send(calibration[hand.calibrationStep].action);
            break;
        case cancelPose: send(Action::cancelHand); break;
        case centerStick: send(Action::joystickCenter); break;
        case finishStick: send(Action::joystickFinish); break;
        case pair: send(Action::pairStart); break;
        case stopPair: send(bothPairing_ ? Action::pairBothStop : Action::pairStop); bothPairing_ = false; break;
        case pairBoth: send(Action::pairBoth); bothPairing_ = true; break;
        case setChannel: {
            BOOL valid = FALSE;
            const auto channel = GetDlgItemInt(window_, channelValue, &valid, FALSE);
            if (valid && channel <= 100) send(Action::channel, static_cast<int>(channel));
            else status_ = L"Enter a channel between 0 and 100.";
            break;
        }
        case vibrate: send(Action::haptic, 0, .12f, .8f); break;
        case useHand: settings.enabled = IsDlgButtonChecked(window_, id) == BST_CHECKED; save(); break;
        case useJoystick: settings.joystick = IsDlgButtonChecked(window_, id) == BST_CHECKED; save(); break;
        case useHaptics: settings.haptics = IsDlgButtonChecked(window_, id) == BST_CHECKED; save(); break;
        case applyPose:
            try {
                auto next = settings;
                const auto tracker = field(trackerNum);
                next.tracker = tracker == L"Auto" || tracker == L"auto" ? noTracker : parse<unsigned>(tracker);
                next.position = {parse<float>(field(posX)), parse<float>(field(posY)), parse<float>(field(posZ))};
                next.rotation = {parse<float>(field(rotX)), parse<float>(field(rotY)), parse<float>(field(rotZ))};
                if (!validPose(next)) throw std::invalid_argument("Pose out of range");
                settings = next;
                save();
            } catch (const std::exception&) { status_ = L"Enter Auto or a device index from 1 to 63 and valid offsets."; }
            break;
        case applyStream:
            try {
                const auto address = field(udpIp);
                std::string ip;
                for (const auto character : address) {
                    if ((character < L'0' || character > L'9') && character != L'.')
                        throw std::invalid_argument("Invalid IPv4 character");
                    ip.push_back(static_cast<char>(character));
                }
                StreamSettings next{std::move(ip), parse<unsigned>(field(vmcPort)), parse<unsigned>(field(oscPort))};
                if (!stream_.configure(next)) throw std::invalid_argument("Invalid endpoint");
                prefs_.stream = std::move(next);
                save();
            } catch (const std::exception&) { status_ = L"Enter an IPv4 address and ports from 0 to 65535."; }
            break;
        case globalHotkey:
            prefs_.hotkey = IsDlgButtonChecked(window_, id) == BST_CHECKED;
            if (prefs_.hotkey) {
                if (!RegisterHotKey(window_, 1, MOD_NOREPEAT, VK_F1)) {
                    prefs_.hotkey = false;
                    CheckDlgButton(window_, id, BST_UNCHECKED);
                    status_ = L"F1 is already registered by another app.";
                    break;
                }
            } else UnregisterHotKey(window_, 1);
            save();
            break;
        case saveCsv: exportCsv(); break;
        case dashboard: {
            const auto exe = executableFolder() / "OpenCapDashboard.exe";
            const auto rc = ShellExecuteW(window_, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(rc) <= 32) status_ = L"Could not start the dashboard executable.";
            break;
        }
        }
    }

    void send(Action action, int value = 0, float duration = 0, float amplitude = 0) {
        if (!bus_.send({static_cast<Side>(side_), action, value, duration, amplitude}))
            status_ = L"The command queue is busy.";
    }

    void tick() {
        const auto time = now();
        for (unsigned i = 0; i < 32; ++i) {
            const auto command = bus_.next();
            if (!command) break;
            const auto s = static_cast<std::size_t>(command->side);
            if (s >= handCount) continue;
            if (command->action == Action::toggleStick) {
                prefs_.hands[s].joystick = !prefs_.hands[s].joystick;
                if (side_ == s) CheckDlgButton(window_, useJoystick, prefs_.hands[s].joystick ? BST_CHECKED : BST_UNCHECKED);
                save();
                continue;
            }
            if (command->action == Action::adjustPose) {
                if (command->value < 0 || command->value > 5
                    || (command->amplitude != -1 && command->amplitude != 1)) continue;
                auto& h = prefs_.hands[s];
                const std::array fields{&h.position.x, &h.position.y, &h.position.z,
                                        &h.rotation.x, &h.rotation.y, &h.rotation.z};
                const bool rotation = command->value >= 3;
                const auto limit = rotation ? 360.0f : 1.0f;
                auto& field = *fields[command->value];
                field = std::clamp(field + command->amplitude * (rotation ? 1.0f : .001f), -limit, limit);
                if (side_ == s) select(s);
                save();
                continue;
            }
            if (demo_) {
                auto& step = demoSteps_[s];
                switch (command->action) {
                case Action::startHand: step = 1; break;
                case Action::fist: if (step == 1) step = 2; break;
                case Action::together: if (step == 2) step = 3; break;
                case Action::spread:
                case Action::cancelHand: if (step && step <= 3) step = 0; break;
                case Action::joystickCenter: step = 4; break;
                case Action::joystickFinish: if (step == 4) step = 0; break;
                default: break;
                }
                continue;
            }
#ifdef OPENCAP_RECEIVER
            if (receiver_ && snapshot_.hands[s].receiver) {
                if (!receiver_->submit(*command)) status_ = L"The receiver command queue is busy.";
                continue;
            }
#endif
            status_ = L"Connect a receiver for this hand first.";
        }
#ifdef OPENCAP_RECEIVER
        if (receiver_) snapshot_.hands = receiver_->read();
#endif
        if (demo_) {
            for (std::size_t s = 0; s < handCount; ++s) {
                snapshot_.hands[s] = demoHand(static_cast<Side>(s), time);
                snapshot_.hands[s].calibrationStep = demoSteps_[s];
            }
        }
        snapshot_.heartbeat = time;
        snapshot_.settings = prefs_.hands;
        bus_.publish(snapshot_);
        stream_.send(snapshot_);
        for (std::size_t s = 0; s < handCount; ++s) {
            const auto count = snapshot_.hands[s].calibrationRequests;
            if (count > lastRequests_[s] && snapshot_.hands[s].calibrationStep == 0)
                bus_.send({static_cast<Side>(s), Action::startHand});
            lastRequests_[s] = count;
        }
        const auto stage = snapshot_.hands[side_].calibrationStep;
        if (lastStage_ != stage) {
            lastStage_ = stage;
            SetDlgItemTextW(window_, nextPose, calibration[(std::min)(stage, 4u)].label);
            EnableWindow(GetDlgItem(window_, nextPose), stage != 4);
            EnableWindow(GetDlgItem(window_, cancelPose), stage >= 1 && stage <= 3);
            EnableWindow(GetDlgItem(window_, finishStick), stage == 4);
        }
        if (time - lastPaint_ >= 50) {
            lastPaint_ = time;
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void exportCsv() {
        const auto& hand = snapshot_.hands[side_];
        if (!isLive(snapshot_, static_cast<Side>(side_), now())) {
            status_ = L"Connect the selected glove before exporting.";
            return;
        }
        wchar_t destination[MAX_PATH] = L"opencap-hand.csv";
        OPENFILENAMEW request{};
        request.lStructSize = sizeof(request);
        request.hwndOwner = window_;
        request.lpstrFile = destination;
        request.nMaxFile = MAX_PATH;
        request.lpstrFilter = L"CSV file\0*.csv\0\0";
        request.lpstrDefExt = L"csv";
        request.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&request)) return;
        std::ofstream output{std::filesystem::path(destination)};
        output.imbue(std::locale::classic());
        output << "hand,finger,joint,curl,qx,qy,qz,qw\n";
        for (std::size_t i = 0; i < hand.joints.size(); ++i) {
            const auto q = hand.joints[i].rotation;
            output << (side_ ? "right" : "left") << ',' << i/3 << ',' << i%3 << ','
                   << hand.curl[i/3] << ',' << q.x << ',' << q.y << ',' << q.z << ',' << q.w << '\n';
        }
        output.close();
        status_ = output ? L"Hand sample exported." : L"Could not write the export.";
    }

    void text(HDC dc, int x, int y, std::wstring_view value, COLORREF color = ink) const {
        SetTextColor(dc, color);
        TextOutW(dc, x, y, value.data(), static_cast<int>(value.size()));
    }

    void drawHand(HDC dc, const Hand& hand, bool active) const {
        const auto pen = CreatePen(PS_SOLID, 5, active ? accent : RGB(156, 170, 180));
        const auto old = SelectObject(dc, pen);
        constexpr std::array starts{POINT{136, 396}, POINT{155, 368}, POINT{182, 360}, POINT{209, 369}, POINT{233, 383}};
        constexpr std::array length{30.0f, 37.0f, 41.0f, 37.0f, 30.0f};
        for (std::size_t i = 0; i < starts.size(); ++i) {
            auto x = static_cast<float>(starts[i].x);
            auto y = static_cast<float>(starts[i].y);
            const float bend = active ? hand.curl[i] : 0;
            const float splay = (static_cast<float>(i) - 2) * .18f;
            MoveToEx(dc, 190, 451, nullptr);
            LineTo(dc, static_cast<int>(x), static_cast<int>(y));
            for (unsigned j = 0; j < 3; ++j) {
                const auto flex = bend * static_cast<float>(j + 1);
                const auto segment = length[i] * (1.0f - j * .17f);
                x += segment * (std::sin(splay) + flex * .15f);
                y -= segment * std::cos(flex * .82f);
                LineTo(dc, static_cast<int>(x), static_cast<int>(y));
            }
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void paint() {
        PAINTSTRUCT paint{};
        const auto target = BeginPaint(window_, &paint);
        const auto dc = CreateCompatibleDC(target);
        const auto bitmap = CreateCompatibleBitmap(target, winWidth, winHeight);
        const auto oldImage = SelectObject(dc, bitmap);
        RECT rect{0, 0, winWidth, winHeight};
        FillRect(dc, &rect, brush_);
        const auto oldFont = SelectObject(dc, title_);
        SetBkMode(dc, TRANSPARENT);
        text(dc, 30, 19, L"OpenCap");
        SelectObject(dc, font_);
        text(dc, 30, 58, demo_ ? L"Simulated gloves" : L"UDCAP glove control", accent);
        for (std::size_t i = 0; i < handCount; ++i) {
            const auto& hand = snapshot_.hands[i];
            const auto x = 30 + static_cast<int>(i) * 425;
            const auto live = isLive(snapshot_, static_cast<Side>(i), now());
            const auto link = live ? L"Streaming" : hand.receiver ? L"Receiver connected · glove offline" : L"No receiver";
            text(dc, x, 101, std::format(L"{}  /  {}", sideNames[i], link), live ? accent : ink);
            const auto battery = hand.battery >= 0 ? std::format(L"{:.0f}%", 100 * hand.battery) : L"—";
            const auto signal = hand.signal ? std::format(L"{} dBm", hand.signal) : L"—";
            const auto firmware = hand.firmware[0] ? widen(hand.firmware.data()) : L"—";
            text(dc, x, 124, std::format(L"Battery {}   ·   {:.0f} fps   ·   {}   ·   FW {}", battery, hand.fps, signal, firmware));
        }
        const auto& hand = snapshot_.hands[side_];
        const auto active = isLive(snapshot_, static_cast<Side>(side_), now());
        text(dc, 30, 211, L"LIVE HAND", accent);
        drawHand(dc, hand, active);
        text(dc, 30, 476, std::format(L"Trigger {:.0f}%   Grip {:.0f}%   Trackpad {:.0f}%",
             100*hand.trigger, 100*hand.grip, 100*hand.pad));
        text(dc, 30, 498, std::format(L"Stick {:+.2f}, {:+.2f}   ·   Buttons 0x{:02X}", hand.stickX, hand.stickY, hand.buttons));
        text(dc, 422, 208, hand.calibrated ? L"CALIBRATION  /  READY" : L"CALIBRATION", accent);
        text(dc, 422, 363, hand.channel >= 0 ? std::format(L"Channel {}", hand.channel) : L"Channel —");
        text(dc, 422, 451, L"TRACKER ALIGNMENT", accent);
        text(dc, 422, 474, L"Index     Position X / Y / Z (m)             Rotation X / Y / Z (°)");
        const auto actionText = hand.calibrationStep == 4 ? L"Move the joystick around its full range, then select Finish."
            : hand.pairing ? L"Pairing is active. Put the glove into pairing mode." : L"";
        const auto message = !status_.empty() ? status_ : hand.message[0] ? widen(hand.message.data())
            : *actionText ? actionText
            : !demo_ && !snapshot_.receiverSupported ? L"This build supports simulated input. Physical receiver support is disabled." : L"";
        text(dc, 30, 577, message, accent);
        text(dc, 30, 610, L"STREAM OUTPUT", accent);
        text(dc, 30, 632, L"Destination IPv4");
        text(dc, 226, 632, L"VMC port");
        text(dc, 332, 632, L"OSC port");
        text(dc, 30, 695, L"Port 0 turns the stream off.");
        BitBlt(target, 0, 0, winWidth, winHeight, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldImage);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window_, &paint);
    }

    bool demo_;
    std::filesystem::path folder_;
    Preferences prefs_{};
    Snapshot snapshot_{};
    Bus bus_;
    Stream stream_;
#ifdef OPENCAP_RECEIVER
    std::unique_ptr<Receiver> receiver_;
#endif
    HWND window_{};
    HFONT font_{}, title_{};
    HBRUSH brush_{};
    std::size_t side_{};
    unsigned lastStage_{UINT32_MAX};
    Millis lastPaint_{};
    std::array<unsigned, handCount> demoSteps_{}, lastRequests_{};
    std::wstring status_;
    bool bothPairing_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR line, int show) {
    const auto args = std::wstring_view(line);
    const bool demo = args == L"--demo";
    const auto lock = CreateMutexW(nullptr, FALSE, L"Local\\OpenCap.Desktop.v1");
    if (!lock) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const auto previous = FindWindowW(L"OpenCapDesktop", nullptr);
        if (previous) { ShowWindow(previous, SW_RESTORE); SetForegroundWindow(previous); }
        CloseHandle(lock);
        return 0;
    }
    int result = 0;
    try {
        Desktop app(demo);
        result = app.run(instance, show);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, widen(error.what()).c_str(), L"OpenCap", MB_ICONERROR);
        result = 1;
    }
    CloseHandle(lock);
    return result;
}
