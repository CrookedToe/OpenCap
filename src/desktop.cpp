#include "bridge.hpp"
#include "desktop_preferences.hpp"
#include "desktop_view.hpp"
#include "hand_view.hpp"
#include "input_dialog.hpp"
#include "stream.hpp"
#include "udcap_preferences.hpp"
#ifdef OPENCAP_RECEIVER
#include "receiver.hpp"
#endif

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <sapi.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <locale>
#include <sstream>
#include <string_view>

namespace {
using namespace opencap;

constexpr UINT_PTR tickTimer = 1;
enum Control {
    leftHand = 100, rightHand, nextPose, cancelPose, centerStick, finishStick, pair,
    stopPair, pairBoth, channelValue, setChannel, vibrate, useHand, useJoystick, useHaptics,
    trackerNum, posX, posY, posZ, rotX, rotY, rotZ, applyPose, udpIp, vmcPort, oscPort,
    applyStream, saveCsv, dashboard, globalHotkey, handView, inputMapping, preferOpenCap, jointAdjustment,
    oscIp, importUdcap, quickJoystick, trackingPlan, useTrackingPlan, inspectImage, steamStartup
};

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
constexpr std::array guidance{L"Make a fist.", L"Straighten your fingers and bring them together.",
                              L"Spread your fingers apart."};

class Voice {
public:
    explicit Voice(bool enabled) {
        if (!enabled) return;
        initialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        if (initialized_)
            CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&voice_));
    }
    ~Voice() {
        if (voice_) voice_->Release();
        if (initialized_) CoUninitialize();
    }
    void say(const wchar_t* words) const {
        if (voice_) voice_->Speak(words, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
private:
    bool initialized_{};
    ISpVoice* voice_{};
};

class Desktop {
public:
    Desktop(bool demo, const std::wstring& session, bool isolated, HANDLE steamOverlay)
        : demo_(demo), folder_(isolated ? std::filesystem::path{} : configFolder(demo)),
          prefs_(loadSettings(folder_)), bus_(session), voice_(!isolated), steamOverlay_(steamOverlay) {
        if (!bus_.reset()) throw std::runtime_error("Could not initialize the OpenCap session.");
        snapshot_.demo = demo_;
        UdcapPreferences vendor;
        if (!demo_ && !folder_.empty()) vendor = readUdcapPreferences(findUdcapSources(folder_.parent_path()));
        const bool firstImport = vendor.found && !prefs_.udcapImport;
        if (firstImport) {
            applyUdcapPreferences(vendor, prefs_.hands, prefs_.stream);
            prefs_.quickStick = vendor.quickStick;
            prefs_.udcapImport = nextImport(0);
            saveSettings(folder_, prefs_);
        }
        if (!stream_.configure(prefs_.stream)) {
            prefs_.stream = {};
            stream_.configure(prefs_.stream);
        }
        snapshot_.settings = prefs_.hands;
#ifdef OPENCAP_RECEIVER
        snapshot_.receiverSupported = true;
        if (!demo_ && !folder_.empty())
            receiver_ = std::make_unique<Receiver>(folder_ / "Gloves", std::move(vendor.receiver), prefs_.udcapImport);
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
        RECT area{0, 0, desktopWidth, desktopHeight};
        AdjustWindowRect(&area, style, FALSE);
        window_ = CreateWindowW(cls.lpszClassName, L"OpenCap", style, CW_USEDEFAULT, CW_USEDEFAULT,
            area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        ShowWindow(window_, view);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) startQuick((1u << handCount) - 1);
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
            if (LOWORD(wParam) == trackerNum && HIWORD(wParam) == CBN_DROPDOWN) listTrackers();
            else if (HIWORD(wParam) == BN_CLICKED) click(LOWORD(wParam));
            return 0;
        case WM_TIMER: tick(); return 0;
        case WM_HOTKEY: if (wParam == 1) startQuick((1u << handCount) - 1); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paintDesktop(window_, brush_, font_, title_, snapshot_, tracking_, side_, status_); return 0;
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
        brush_ = CreateSolidBrush(desktopBackground);
        button(leftHand, L"Left glove", 30, 161, 130, BS_AUTORADIOBUTTON | WS_GROUP);
        button(rightHand, L"Right glove", 170, 161, 130, BS_AUTORADIOBUTTON);
        button(quickJoystick, L"Hold stick to toggle joystick", 307, 161, 248, WS_GROUP | BS_AUTOCHECKBOX);
        CheckDlgButton(window_, quickJoystick, prefs_.quickStick ? BST_CHECKED : BST_UNCHECKED);
        button(dashboard, L"Open in SteamVR", 707, 27, 182);
        if (!demo_) button(importUdcap, L"Import UDCAP settings", 494, 27, 201);
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
        button(inputMapping, L"Controller…", 755, 444, 134);
        const auto tracker = item(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL, 422, 497, 110, 210, trackerNum);
        SendMessageW(tracker, CB_SETDROPPEDWIDTH, 280, 0);
        edit(posX, 538, 497, 50); edit(posY, 595, 497, 51); edit(posZ, 652, 497, 50);
        edit(rotX, 709, 497, 52); edit(rotY, 767, 497, 53); edit(rotZ, 827, 497, 53);
        button(applyPose, L"Apply tracker alignment", 422, 531, 228);
        button(saveCsv, L"Export hand sample", 30, 528, 181);
        button(handView, L"Open hand viewer", 223, 528, 170);
        button(jointAdjustment, L"Joint correction…", 223, 205, 170);
        button(globalHotkey, L"F1 quick calibration", 662, 531, 227, BS_AUTOCHECKBOX);
        button(preferOpenCap, L"Prefer OpenCap in SteamVR", 662, 567, 227, BS_AUTOCHECKBOX);
        const auto plans = item(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 422, 568, 137, 194, trackingPlan);
        for (const auto& preset : trackingPresets)
            SendMessageW(plans, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.name));
        SendMessageW(plans, CB_SETCURSEL, 0, 0);
        button(useTrackingPlan, L"Use preset", 568, 566, 88);
        edit(udpIp, 30, 654, 174);
        edit(vmcPort, 215, 654, 77);
        edit(oscIp, 314, 654, 174);
        edit(oscPort, 499, 654, 77);
        button(applyStream, L"Apply streaming", 592, 651, 167);
        button(inspectImage, L"Inspect firmware image…", 683, 716, 206);
        if (!demo_) {
            button(steamStartup, L"Start and stop with SteamVR", 30, 720, 254, BS_AUTOCHECKBOX);
            CheckDlgButton(window_, steamStartup, prefs_.steamLifecycle ? BST_CHECKED : BST_UNCHECKED);
        }
        showStream();
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
        CheckDlgButton(window_, preferOpenCap, h.highPriority ? BST_CHECKED : BST_UNCHECKED);
        const auto trackerText = h.trackerSerial[0] ? L"ID " + widen(h.trackerSerial.data())
            : h.tracker == noTracker ? L"Auto" : std::to_wstring(h.tracker);
        SetDlgItemTextW(window_, trackerNum, trackerText.c_str());
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

    void listTrackers() {
        const auto previous = field(trackerNum);
        SendDlgItemMessageW(window_, trackerNum, CB_RESETCONTENT, 0, 0);
        SendDlgItemMessageW(window_, trackerNum, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        if (trackingCurrent(tracking_, now())) {
            for (unsigned i = 0; i < tracking_.count && i < tracking_.devices.size(); ++i) {
                const auto label = L"ID " + widen(tracking_.devices[i].serial.data());
                SendDlgItemMessageW(window_, trackerNum, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        SetDlgItemTextW(window_, trackerNum, previous.c_str());
    }

    template <class Number>
    static Number parse(const std::wstring& field) {
        std::wistringstream text(field);
        text.imbue(std::locale::classic());
        Number value{};
        if (!(text >> value) || (text >> std::ws && !text.eof())) throw std::invalid_argument("Invalid field");
        return value;
    }

    std::string readIp(int id) const {
        std::string result;
        for (const auto character : field(id)) {
            if ((character < L'0' || character > L'9') && character != L'.')
                throw std::invalid_argument("Invalid IPv4 character");
            result.push_back(static_cast<char>(character));
        }
        return result;
    }

    void showStream() {
        SetDlgItemTextW(window_, udpIp, widen(prefs_.stream.address).c_str());
        SetDlgItemTextW(window_, oscIp, widen(prefs_.stream.oscAddress).c_str());
        SetDlgItemInt(window_, vmcPort, prefs_.stream.vmcPort, FALSE);
        SetDlgItemInt(window_, oscPort, prefs_.stream.oscPort, FALSE);
    }

    void importSettings() {
        if (quickMask_ || std::ranges::any_of(snapshot_.hands, [](const auto& h) { return h.calibrationStep != 0; })) {
            status_ = L"Finish the current calibration before importing settings.";
            return;
        }
        if (folder_.empty()) { status_ = L"OpenCap's settings folder is unavailable."; return; }
        auto source = readUdcapPreferences(findUdcapSources(folder_.parent_path()));
        if (!source.found) { status_ = L"No UDCAP settings were found."; return; }
        auto next = prefs_;
        applyUdcapPreferences(source, next.hands, next.stream);
        next.quickStick = source.quickStick;
        next.udcapImport = nextImport(prefs_.udcapImport);
        if (!saveSettings(folder_, next)) { status_ = L"Could not save the imported settings."; return; }
        prefs_ = std::move(next);
        stream_.configure(prefs_.stream);
#ifdef OPENCAP_RECEIVER
        if (receiver_) receiver_->importPreferences(std::move(source.receiver), prefs_.udcapImport);
#endif
        select(side_);
        CheckDlgButton(window_, quickJoystick, prefs_.quickStick ? BST_CHECKED : BST_UNCHECKED);
        showStream();
        status_ = source.saved ? L"Saved UDCAP settings imported." : L"UDCAP factory defaults imported.";
    }

    void save() {
        status_ = saveSettings(folder_, prefs_) ? L"Settings saved." : L"Could not save settings.";
    }

    void click(int id) {
        auto& settings = prefs_.hands[side_];
        const auto& hand = snapshot_.hands[side_];
        switch (id) {
        case importUdcap: importSettings(); break;
        case inspectImage: showFirmwareImage(window_); break;
        case steamStartup:
            prefs_.steamLifecycle = IsDlgButtonChecked(window_, id) == BST_CHECKED;
            save();
            break;
        case useTrackingPlan:
            usePreset(static_cast<std::size_t>(SendDlgItemMessageW(window_, trackingPlan, CB_GETCURSEL, 0, 0)));
            break;
        case leftHand: select(0); break;
        case rightHand: select(1); break;
        case nextPose:
            if (!quickMask_ && hand.calibrationStep < 4) send(calibration[hand.calibrationStep].action);
            break;
        case cancelPose: if (quickMask_) stopQuick(); else send(Action::cancelHand); break;
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
        case preferOpenCap: settings.highPriority = IsDlgButtonChecked(window_, id) == BST_CHECKED; save(); break;
        case quickJoystick: prefs_.quickStick = IsDlgButtonChecked(window_, id) == BST_CHECKED; save(); break;
        case inputMapping: {
            const auto side = static_cast<Side>(side_);
            const auto previousRanges = hand.buttonRanges;
            const auto connected = isLive(snapshot_, side, now());
            if (const auto edit = editInputs(window_, settings, hand, connected)) {
                if (edit->ranges != previousRanges &&
                    (!isLive(snapshot_, side, now()) ||
                     !bus_.send(Command{.side = side, .action = Action::setButtonRanges, .ranges = edit->ranges}))) {
                    status_ = L"Connect the glove and try saving again.";
                    break;
                }
                settings.buttonMap = edit->mapping;
                settings.triggerEnabled = edit->enabled[0];
                settings.gripEnabled = edit->enabled[1];
                settings.padEnabled = edit->enabled[2];
                settings.thumbEnabled = edit->enabled[3];
                save();
            }
            break;
        }
        case jointAdjustment:
            if (!isLive(snapshot_, static_cast<Side>(side_), now()) || !hand.calibrated) {
                status_ = L"Connect and calibrate this glove before adjusting its joints.";
                break;
            }
            {
                const auto current = hand.jointOffsets;
                if (const auto joint = editJoint(window_, current)) {
                    const auto [x, y, z] = joint->offset;
                    if (!bus_.send({static_cast<Side>(side_), Action::setJointOffset, joint->joint, x, y, z}))
                        status_ = L"The command queue is busy.";
                }
            }
            break;
        case applyPose:
            try {
                auto next = settings;
                auto tracker = field(trackerNum);
                next.tracker = noTracker;
                next.trackerSerial.fill(0);
                if (!tracker.empty() && tracker != L"Auto" && tracker != L"auto") {
                    const bool explicitSerial = tracker.starts_with(L"ID ");
                    if (explicitSerial) tracker.erase(0, 3);
                    if (!explicitSerial && tracker.find_first_not_of(L"0123456789") == std::wstring::npos)
                        next.tracker = parse<unsigned>(tracker);
                    else {
                        if (tracker.size() >= next.trackerSerial.size()) throw std::invalid_argument("Tracker serial is too long");
                        for (std::size_t i = 0; i < tracker.size(); ++i) {
                            if (tracker[i] < 32 || tracker[i] > 126) throw std::invalid_argument("Invalid tracker serial");
                            next.trackerSerial[i] = static_cast<char>(tracker[i]);
                        }
                    }
                }
                next.position = {parse<float>(field(posX)), parse<float>(field(posY)), parse<float>(field(posZ))};
                next.rotation = {parse<float>(field(rotX)), parse<float>(field(rotY)), parse<float>(field(rotZ))};
                if (!validPose(next)) throw std::invalid_argument("Pose out of range");
                settings = next;
                save();
            } catch (const std::exception&) { status_ = L"Enter Auto, a tracker serial, or a device index from 1 to 63, and valid offsets."; }
            break;
        case applyStream:
            try {
                StreamSettings next{readIp(udpIp), parse<unsigned>(field(vmcPort)), parse<unsigned>(field(oscPort)), readIp(oscIp)};
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
        case handView:
            if (!openHandView(window_, static_cast<unsigned>(side_))) status_ = L"Could not open the hand viewer.";
            break;
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
        else status_.clear();
    }

    void toggleStick(std::size_t side) {
        auto& settings = prefs_.hands[side];
        settings.joystick = !settings.joystick;
        if (side_ == side) CheckDlgButton(window_, useJoystick, settings.joystick ? BST_CHECKED : BST_UNCHECKED);
        if (saveSettings(folder_, prefs_))
            status_ = settings.joystick ? L"Joystick enabled." : L"Joystick disabled.";
        else status_ = L"Joystick changed but the setting could not be saved.";
        if (settings.haptics && isLive(snapshot_, static_cast<Side>(side), now()))
            bus_.send({static_cast<Side>(side), Action::haptic, 0, .12f, .75f});
    }

    void usePreset(std::size_t preset) {
        if (!applyTrackingPreset(prefs_.hands, preset)) return;
        SendDlgItemMessageW(window_, trackingPlan, CB_SETCURSEL, preset, 0);
        select(side_);
        if (saveSettings(folder_, prefs_))
            status_ = std::format(L"Applied {} alignment to both hands.", trackingPresets[preset].name);
        else status_ = L"Alignment was applied but could not be saved.";
    }

    void startQuick(unsigned requested) {
        if (quickMask_) return;
        const auto time = now();
        for (std::size_t i = 0; i < handCount; ++i) {
            const auto bit = 1u << i;
            const auto side = static_cast<Side>(i);
            if ((requested & bit) && isLive(snapshot_, side, time) && snapshot_.hands[i].calibrationStep == 0
                && bus_.send({side, Action::startHand})) quickMask_ |= bit;
        }
        if (!quickMask_) {
            status_ = L"Connect a glove and finish any current calibration.";
            return;
        }
        quickPhase_ = 1;
        quickDeadline_ = time + 4500;
        quickDue_ = 0;
        status_ = L"Starting quick calibration.";
    }

    void stopQuick() {
        for (std::size_t i = 0; i < handCount; ++i)
            if (quickMask_ & (1u << i)) bus_.send({static_cast<Side>(i), Action::cancelHand});
        quickMask_ = 0;
        status_ = L"Quick calibration stopped.";
    }

    void guide(Millis time) {
        if (!quickMask_) return;
        bool ready = true;
        for (std::size_t i = 0; i < handCount; ++i) {
            if (!(quickMask_ & (1u << i))) continue;
            if (!isLive(snapshot_, static_cast<Side>(i), time)) { stopQuick(); return; }
            if (snapshot_.hands[i].calibrationStep != quickPhase_ % 4) ready = false;
        }
        if (!ready) {
            if (time >= quickDeadline_) stopQuick();
            return;
        }
        if (quickPhase_ == 4) {
            quickMask_ = 0;
            status_ = L"Quick calibration complete.";
            voice_.say(L"Calibration complete.");
            return;
        }
        if (!quickDue_) {
            status_ = guidance[quickPhase_ - 1];
            voice_.say(guidance[quickPhase_ - 1]);
            quickDue_ = time + 3500;
            return;
        }
        if (time < quickDue_) return;
        for (std::size_t i = 0; i < handCount; ++i)
            if (quickMask_ & (1u << i)) bus_.send({static_cast<Side>(i), calibration[quickPhase_].action});
        ++quickPhase_;
        quickDeadline_ = time + 4500;
        quickDue_ = 0;
    }

    void tick() {
        if (steamOverlay_ && WaitForSingleObject(steamOverlay_, 0) == WAIT_OBJECT_0) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        const auto time = now();
        for (unsigned i = 0; i < 32; ++i) {
            const auto command = bus_.next();
            if (!command) break;
            const auto s = static_cast<std::size_t>(command->side);
            if (s >= handCount) continue;
            if (command->action == Action::toggleStick) {
                toggleStick(s);
                continue;
            }
            if (command->action == Action::applyPreset) {
                usePreset(static_cast<std::size_t>(command->value));
                continue;
            }
            if (command->action == Action::guidedCalibration) {
                startQuick(1u << s);
                continue;
            }
            if (command->action == Action::cancelHand && (quickMask_ & (1u << s))) {
                stopQuick();
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
                case Action::startHand: if (step != 4) step = 1; break;
                case Action::fist: if (step == 1) step = 2; break;
                case Action::together: if (step == 2) step = 3; break;
                case Action::spread:
                case Action::cancelHand: if (step && step <= 3) step = 0; break;
                case Action::joystickCenter: if (!step || step == 4) step = 4; break;
                case Action::joystickFinish: if (step == 4) step = 0; break;
                case Action::setJointOffset: {
                    const Vec3 offset{command->duration, command->amplitude, command->extra};
                    const auto joint = static_cast<std::size_t>(command->value);
                    if (joint < jointCount && validJointOffset(offset)) demoOffsets_[s][joint] = offset;
                    break;
                }
                case Action::setButtonRanges:
                    if (std::ranges::all_of(command->ranges, validButtonRange)) demoRanges_[s] = command->ranges;
                    break;
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
                snapshot_.hands[s].jointOffsets = demoOffsets_[s];
                snapshot_.hands[s].buttonRanges = demoRanges_[s];
                for (std::size_t j = 0; j < jointCount; ++j) {
                    auto& rotation = snapshot_.hands[s].joints[j].rotation;
                    rotation = multiply(rotation, eulerDegrees(demoOffsets_[s][j]));
                }
            }
        }
        snapshot_.heartbeat = now();
        for (std::size_t s = 0; s < handCount; ++s) {
            const auto& hand = snapshot_.hands[s];
            const bool held = prefs_.quickStick && isLive(snapshot_, static_cast<Side>(s), snapshot_.heartbeat)
                && (hand.buttons & bit(Button::stick));
            if (stickHolds_[s].update(held, snapshot_.heartbeat)) toggleStick(s);
        }
        snapshot_.settings = prefs_.hands;
        for (std::size_t s = 0; s < handCount; ++s)
            applyHandPreferences(snapshot_.hands[s], snapshot_.settings[s]);
        bus_.publish(snapshot_);
        stream_.send(snapshot_);
        for (std::size_t s = 0; s < handCount; ++s) {
            const auto count = snapshot_.hands[s].calibrationRequests;
            if (count > lastRequests_[s] && snapshot_.hands[s].calibrationStep == 0)
                startQuick(1u << s);
            lastRequests_[s] = count;
        }
        guide(snapshot_.heartbeat);
        const auto stage = snapshot_.hands[side_].calibrationStep;
        if (lastStage_ != stage) {
            lastStage_ = stage;
            SetDlgItemTextW(window_, nextPose, calibration[(std::min)(stage, 4u)].label);
            EnableWindow(GetDlgItem(window_, nextPose), stage != 4);
            EnableWindow(GetDlgItem(window_, cancelPose), stage >= 1 && stage <= 3);
            EnableWindow(GetDlgItem(window_, centerStick), stage == 0 || stage == 4);
            EnableWindow(GetDlgItem(window_, finishStick), stage == 4);
        }
        if (time - lastPaint_ >= 50) {
            if (const auto update = bus_.readTracking()) tracking_ = *update;
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
        if (const auto exported = exportHandSample(window_, static_cast<Side>(side_), hand))
            status_ = *exported ? L"Hand sample exported." : L"Could not write the export.";
    }

    bool demo_;
    std::filesystem::path folder_;
    Preferences prefs_{};
    Snapshot snapshot_{};
    TrackingSnapshot tracking_{};
    Bus bus_;
    Stream stream_;
    Voice voice_;
    HANDLE steamOverlay_{};
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
    std::array<std::array<Vec3, jointCount>, handCount> demoOffsets_{};
    std::array<ButtonRanges, handCount> demoRanges_{defaultButtonRanges, defaultButtonRanges};
    std::array<StickHold, handCount> stickHolds_{};
    std::wstring status_;
    bool bothPairing_{};
    unsigned quickMask_{}, quickPhase_{};
    Millis quickDue_{}, quickDeadline_{};
};
}

int opencap::runDesktop(HINSTANCE instance, int show, bool demo, const std::wstring& session, bool isolated, HANDLE steamOverlay) {
    Desktop app(demo, session, isolated, steamOverlay);
    return app.run(instance, show);
}
