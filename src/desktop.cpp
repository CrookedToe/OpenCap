#include "bridge.hpp"
#include "desktop_preferences.hpp"
#include "desktop_view.hpp"
#include "hand_view.hpp"
#include "input_dialog.hpp"
#include "stream.hpp"
#include "udcap_preferences.hpp"
#ifdef OPENCAP_RECEIVER
#include "calibration_log.hpp"
#include "receiver.hpp"
#endif

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <sapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <future>
#include <iterator>
#include <locale>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
using namespace opencap;

constexpr UINT_PTR tickTimer = 1;
constexpr Millis captureTimeout = 5000;




constexpr std::array captureActions{Action::fist, Action::together, Action::spread};
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
        snapshot_.automaticHandCalibration = prefs_.automaticHandCalibration;
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
        if (!demo_ && prefs_.recordCalibration) configureRecording(true);
        if (!demo_ && !folder_.empty())
            receiver_ = std::make_unique<Receiver>(folder_ / "Gloves", std::move(vendor.receiver),
                                                   prefs_.udcapImport, prefs_.automaticHandCalibration, calibrationLog_);
#endif
    }

    ~Desktop() {
        snapshot_.heartbeat = 0;
        snapshot_.hands = {};
        bus_.publish(snapshot_);
        if (font_) DeleteObject(font_);
        if (controlFont_) DeleteObject(controlFont_);
        if (brush_) DeleteObject(brush_);
        if (section_) DeleteObject(section_);
    }

    int run(HINSTANCE instance, int view) {
        WNDCLASSW cls{};
        cls.hInstance = instance;
        cls.lpszClassName = L"OpenCapDesktop";
        cls.lpfnWndProc = dispatch;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&cls)) return 1;
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        const UINT dpi = GetDpiForSystem();
        RECT area{0, 0, MulDiv(1060, dpi, 96), MulDiv(690, dpi, 96)};
        AdjustWindowRectExForDpi(&area, style, FALSE, 0, dpi);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        window_ = CreateWindowW(cls.lpszClassName, L"OpenCap", style, CW_USEDEFAULT, CW_USEDEFAULT,
            std::min(area.right-area.left, work.right-work.left), std::min(area.bottom-area.top, work.bottom-work.top),
            nullptr, nullptr, instance, this);
        if (!window_) return 1;
        ShowWindow(window_, view);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1) startQuick();
            else if (IsDialogMessageW(window_, &msg)) {
                if (msg.message == WM_KEYDOWN) revealFocus();
            } else {
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
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && controlFont_) relayout();
            return 0;
        case WM_GETMINMAXINFO: {
            const auto dpi = GetDpiForWindow(window_);
            RECT min{0, 0, MulDiv(410, dpi, 96), MulDiv(215, dpi, 96)};
            AdjustWindowRectExForDpi(&min, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE, 0, dpi);
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {min.right-min.left, min.bottom-min.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            updateControlFont();
            const auto& box = *reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, box.left, box.top, box.right-box.left, box.bottom-box.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            relayout();
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window_, SB_VERT, &info);
            int target = scroll_;
            switch (LOWORD(wParam)) {
            case SB_TOP: target = 0; break;
            case SB_BOTTOM: target = info.nMax; break;
            case SB_LINEDOWN: target += 32; break;
            case SB_LINEUP: target -= 32; break;
            case SB_PAGEDOWN: target += static_cast<int>(info.nPage); break;
            case SB_PAGEUP: target -= static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: target = info.nTrackPos; break;
            default: return 0;
            }
            scrollTo(target);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
            const int steps = wheelDelta_ / WHEEL_DELTA;
            wheelDelta_ %= WHEEL_DELTA;
            scrollTo(scroll_ - steps * (lines == WHEEL_PAGESCROLL ? clientHeight_ : static_cast<int>(lines) * 16));
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(wParam), desktopBackground);
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(27,35,46));
            return reinterpret_cast<LRESULT>(brush_);
        case WM_COMMAND:
            if (LOWORD(wParam) == trackerNum && HIWORD(wParam) == CBN_DROPDOWN) listTrackers();
            else if (HIWORD(wParam) == BN_CLICKED) click(LOWORD(wParam));
            return 0;
        case WM_TIMER: tick(); return 0;
        case WM_POWERBROADCAST:
#ifdef OPENCAP_RECEIVER
            if (wParam == PBT_APMRESUMEAUTOMATIC && receiver_) {
                if (snapshot_.calibrationGuide.hands) stopQuick(L"Calibration stopped while Windows was asleep.", CalibrationResult::failed);
                receiver_.reset();
                snapshot_.hands = {};
                resumeReceivers();
            }
#endif
            return TRUE;
        case WM_HOTKEY: if (wParam == 1) startQuick(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->code != NM_CUSTOMDRAW || (header->idFrom != leftHand && header->idFrom != rightHand)) break;
            const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
            if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
            DRAWITEMSTRUCT item{};
            item.hwndItem = header->hwndFrom;
            item.hDC = draw.hdc;
            item.rcItem = draw.rc;
            if (draw.uItemState & CDIS_SELECTED) item.itemState |= ODS_SELECTED;
            if (draw.uItemState & CDIS_FOCUS) item.itemState |= ODS_FOCUS;
            paintDesktopButton(item, header->idFrom == (side_ ? rightHand : leftHand)
                ? DesktopButton::selectedHand : DesktopButton::standard);
            return CDRF_SKIPDEFAULT;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType != ODT_BUTTON) break;
            if (wParam >= glovesPage && wParam <= settingsPage) {
                const bool selected = wParam - glovesPage == static_cast<unsigned>(page_);
                paintDesktopButton(item, selected ? DesktopButton::selectedPage : DesktopButton::navigation);
            } else {
                const bool adjustment = wParam == inputMapping || wParam == jointAdjustment || wParam == handResponse;
                paintDesktopButton(item, adjustment ? DesktopButton::adjustment
                    : wParam == handView ? DesktopButton::viewport : DesktopButton::standard);
            }
            return TRUE;
        }
        case WM_CLOSE:
#ifdef OPENCAP_RECEIVER
            if (nativeUpdate_.valid()) return 0;
#endif
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_PAINT: paintDesktop(window_, layout_, scroll_, dpi_, font_, section_, snapshot_, tracking_, side_, page_, status_); return 0;
        case WM_DESTROY:
            UnregisterHotKey(window_, 1);
            KillTimer(window_, tickTimer);
            stream_.configure({});
            PostQuitMessage(0);
            return 0;
        default: break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND item(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        const auto control = CreateWindowExW(style & WS_BORDER ? WS_EX_CLIENTEDGE : 0, type, text,
            WS_CHILD | WS_TABSTOP | (style & ~WS_BORDER), 0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), FALSE);
        return control;
    }

    void button(int id, const wchar_t* caption, DWORD extra = BS_OWNERDRAW) {
        item(L"BUTTON", caption, extra, id);
    }

    void edit(int id) { item(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id); }

    static HFONT makeFont(int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }

    void updateControlFont() {
        const auto previous = controlFont_;
        controlFont_ = makeFont(MulDiv(15, dpi_, 96), FW_NORMAL);
        for (auto child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
        if (previous) DeleteObject(previous);
    }

    void create() {
        dpi_ = GetDpiForWindow(window_);
        font_ = makeFont(15, FW_NORMAL);
        section_ = makeFont(19, FW_SEMIBOLD);
        updateControlFont();
        brush_ = CreateSolidBrush(desktopBackground);

        for (std::size_t i = 0; i < desktopPages.size(); ++i)
            button(glovesPage + static_cast<int>(i), desktopPages[i]);
        button(dashboard, L"Open in SteamVR", BS_OWNERDRAW | WS_GROUP);
        button(leftHand, L"Left glove", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP);
        button(rightHand, L"Right glove", BS_AUTORADIOBUTTON | BS_PUSHLIKE);
        button(handView, L"Orbit view", BS_OWNERDRAW | WS_GROUP);
        button(exportMenu, L"Export…");
        button(nextPose, L"Guided calibration", BS_OWNERDRAW | WS_DISABLED);
        button(cancelPose, L"Cancel");
        button(centerStick, L"Set joystick center");
        button(finishStick, L"Finish joystick range");
        button(inputMapping, L"Controller inputs");
        button(handResponse, L"Hand response");
        button(jointAdjustment, L"Joint correction");

        button(pair, L"Pair glove", BS_OWNERDRAW | WS_GROUP);
        button(stopPair, L"Stop pairing");
        button(pairBoth, L"Pair both");
        button(vibrate, L"Test motor 1");
        button(vibrateSecond, L"Test motor 2");
        button(useJoystick, L"Enable joystick", BS_AUTOCHECKBOX);
        button(useHaptics, L"Enable vibration", BS_AUTOCHECKBOX);
        button(quickJoystick, L"Hold stick to toggle joystick", BS_AUTOCHECKBOX);
        CheckDlgButton(window_, quickJoystick, prefs_.quickStick ? BST_CHECKED : BST_UNCHECKED);

        const auto tracker = item(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_GROUP, trackerNum);
        SendMessageW(tracker, CB_SETDROPPEDWIDTH, 365, 0);
        edit(posX); edit(posY); edit(posZ);
        edit(rotX); edit(rotY); edit(rotZ);
        button(applyPose, L"Apply tracker alignment");
        button(useHand, L"Publish to SteamVR", BS_AUTOCHECKBOX);
        button(preferOpenCap, L"Prefer OpenCap in SteamVR", BS_AUTOCHECKBOX);
        const auto plans = item(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, trackingPlan);
        for (const auto& preset : trackingPresets)
            SendMessageW(plans, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.name));
        SendMessageW(plans, CB_SETCURSEL, 0, 0);
        button(useTrackingPlan, L"Apply to both hands");

        item(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_GROUP, udpIp);
        edit(vmcPort);
        edit(oscIp);
        edit(oscPort);
        button(applyStream, L"Apply streaming");

        button(globalHotkey, L"Allow F1 calibration from other apps", BS_AUTOCHECKBOX | WS_GROUP);
        button(automaticCalibration, L"Automatic hand calibration", BS_AUTOCHECKBOX);
        CheckDlgButton(window_, automaticCalibration, prefs_.automaticHandCalibration ? BST_CHECKED : BST_UNCHECKED);
        button(inspectImage, L"Inspect firmware image");
        if (!demo_) {
            button(steamStartup, L"Start and stop with SteamVR", BS_AUTOCHECKBOX);
            CheckDlgButton(window_, steamStartup, steamLifecycleEnabled(folder_, prefs_) ? BST_CHECKED : BST_UNCHECKED);
#ifdef OPENCAP_RECEIVER
            button(firmwareUpdates, L"Firmware updates");
            button(recordCalibration, L"Record calibration data", BS_AUTOCHECKBOX);
            CheckDlgButton(window_, recordCalibration, prefs_.recordCalibration ? BST_CHECKED : BST_UNCHECKED);
            button(openCalibrationLogs, L"Open log folder");
#endif
            button(checkUpdates, L"Check for updates");
            button(importUdcap, L"Import UDCAP settings");
            button(includeBetas, L"Include beta releases", BS_AUTOCHECKBOX);
            CheckDlgButton(window_, includeBetas, prefs_.includePrereleases ? BST_CHECKED : BST_UNCHECKED);
        }
        showStream();
        if (prefs_.hotkey && !RegisterHotKey(window_, 1, MOD_NOREPEAT, VK_F1)) prefs_.hotkey = false;
        CheckDlgButton(window_, globalHotkey, prefs_.hotkey ? BST_CHECKED : BST_UNCHECKED);
        select(0);
        showPage(DesktopPage::gloves);
        SetTimer(window_, tickTimer, 16, nullptr);
    }

    void showPage(DesktopPage page) {
        page_ = page;
        scroll_ = 0;
        relayout();
        for (int id = glovesPage; id <= settingsPage; ++id) InvalidateRect(GetDlgItem(window_, id), nullptr, TRUE);
    }

    void positionControls() {
        auto batch = BeginDeferWindowPos(settingsPage - leftHand + 1);
        for (int id = leftHand; id <= settingsPage; ++id) {
            const auto child = GetDlgItem(window_, id);
            if (!child) continue;
            const auto& r = layout_.controls[id];
            const bool visible = r.right > r.left;
            const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
            const int x = MulDiv(r.left, dpi_, 96), y = MulDiv(r.top - scroll_, dpi_, 96);
            const int w = MulDiv(r.right, dpi_, 96) - x, h = MulDiv(r.bottom-scroll_, dpi_, 96) - y;
            if (batch) batch = DeferWindowPos(batch, child, nullptr, x, y, w, h, flags);
            else SetWindowPos(child, nullptr, x, y, w, h, flags);
        }
        if (batch) EndDeferWindowPos(batch);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void relayout() {
        if (layingOut_) return;
        layingOut_ = true;
        RECT client{};
        GetClientRect(window_, &client);
        const int bar = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
        const int fullWidth = client.right + ((GetWindowLongPtrW(window_, GWL_STYLE) & WS_VSCROLL) ? bar : 0);
        clientHeight_ = MulDiv(client.bottom, 96, dpi_);
        const auto makeLayout = [&](int width) {
            return desktopLayout(MulDiv(width, 96, dpi_), clientHeight_, page_, demo_,
                                 snapshot_.hands[side_].calibrationStep, snapshot_.calibrationGuide.hands != 0);
        };
        layout_ = makeLayout(fullWidth);
        if (layout_.height > clientHeight_) layout_ = makeLayout(fullWidth - bar);
        scroll_ = std::clamp(scroll_, 0, std::max(0, layout_.height-clientHeight_));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, layout_.height-1,
                        static_cast<UINT>(clientHeight_), scroll_};
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
        positionControls();
        layingOut_ = false;
    }

    void scrollTo(int target) {
        target = std::clamp(target, 0, std::max(0, layout_.height-clientHeight_));
        if (scroll_ == target) return;
        scroll_ = target;
        SetScrollPos(window_, SB_VERT, scroll_, TRUE);
        positionControls();
    }

    void revealFocus() {
        const auto focus = GetFocus();
        if (GetParent(focus) != window_) return;
        const int id = GetDlgCtrlID(focus);
        if (id < leftHand || id > settingsPage) return;
        const auto& rect = layout_.controls[id];
        const int bottom = (id == trackerNum || id == trackingPlan) ? rect.top + 32 : rect.bottom;
        if (rect.top < scroll_) scrollTo(rect.top - 6);
        else if (bottom > scroll_ + clientHeight_) scrollTo(bottom + 6 - clientHeight_);
    }

    void select(std::size_t side) {
        side_ = side;
        CheckRadioButton(window_, leftHand, rightHand, side ? rightHand : leftHand);
        InvalidateRect(GetDlgItem(window_, leftHand), nullptr, TRUE);
        InvalidateRect(GetDlgItem(window_, rightHand), nullptr, TRUE);
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
        if (snapshot_.calibrationGuide.hands ||
            std::ranges::any_of(snapshot_.hands, [](const auto& h) { return h.calibrationStep != 0; })) {
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

#ifdef OPENCAP_RECEIVER
    void configureRecording(bool enabled) {
        try {
            if (enabled) {
                if (folder_.empty()) throw std::runtime_error("OpenCap's settings folder is unavailable.");
                calibrationLog_.start(folder_ / L"Calibration Logs");
            } else calibrationLog_.stop();
            prefs_.recordCalibration = enabled;
            save();
        } catch (const std::exception& error) {
            prefs_.recordCalibration = false;
            saveSettings(folder_, prefs_);
            status_ = widen(error.what());
        }
        if (window_) CheckDlgButton(window_, recordCalibration, prefs_.recordCalibration ? BST_CHECKED : BST_UNCHECKED);
    }
#endif

    void click(int id) {
        if (id >= glovesPage && id <= settingsPage) {
            showPage(static_cast<DesktopPage>(id - glovesPage));
            return;
        }
        auto& settings = prefs_.hands[side_];
        const auto& hand = snapshot_.hands[side_];
        switch (id) {
        case importUdcap: importSettings(); break;
        case inspectImage: showFirmwareImage(window_); break;
#ifdef OPENCAP_RECEIVER
        case firmwareUpdates: firmwareMenu(); break;
        case recordCalibration: configureRecording(IsDlgButtonChecked(window_, id) == BST_CHECKED); break;
        case openCalibrationLogs: {
            const auto directory = folder_ / L"Calibration Logs";
            std::error_code error;
            if (!folder_.empty()) std::filesystem::create_directories(directory, error);
            if (folder_.empty() || error || reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", directory.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32) status_ = L"Could not open the calibration log folder.";
            break;
        }
#endif
        case checkUpdates:
            if (!launchUpdater(false, prefs_.includePrereleases))
                status_ = L"Update checks are available in the packaged Windows build.";
            break;
        case includeBetas:
            prefs_.includePrereleases = IsDlgButtonChecked(window_, id) == BST_CHECKED;
            save();
            break;
        case automaticCalibration:
            prefs_.automaticHandCalibration = IsDlgButtonChecked(window_, id) == BST_CHECKED;
            snapshot_.automaticHandCalibration = prefs_.automaticHandCalibration;
#ifdef OPENCAP_RECEIVER
            if (receiver_) receiver_->setAutomaticHandCalibration(prefs_.automaticHandCalibration);
#endif
            save();
            break;
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
            startQuick();
            break;
        case cancelPose:
            if (snapshot_.calibrationGuide.hands) stopQuick();
            else send(Action::cancelHand, hand.calibrationId);
            break;
        case centerStick: send(Action::joystickCenter); break;
        case finishStick: send(Action::joystickFinish); break;
        case pair: send(Action::pairStart); break;
        case stopPair: send(bothPairing_ ? Action::pairBothStop : Action::pairStop); bothPairing_ = false; break;
        case pairBoth: send(Action::pairBoth); bothPairing_ = true; break;
        case vibrate: send(Action::hapticTest, 0); break;
        case vibrateSecond: send(Action::hapticTest, 1); break;
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
                status_ = L"Connect this glove and move your fingers before adjusting its joints.";
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
        case handResponse:
            openHandResponse();
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
                if (next.tracker != settings.tracker || next.trackerSerial != settings.trackerSerial ||
                    (next.tracker == noTracker && !next.trackerSerial[0] && tracking_.selected[side_] == noTracker))
                    ++next.trackerSelectionRevision;
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
        case exportMenu: {
            const auto menu = CreatePopupMenu();
            if (!menu) break;
            AppendMenuW(menu, MF_STRING | (isLive(snapshot_, static_cast<Side>(side_), now()) ? MF_ENABLED : MF_GRAYED),
                        saveCsv, L"Hand sample (.csv)");
            if (!demo_) AppendMenuW(menu, MF_STRING, saveCalibration, L"Guided calibration (.csv)");
            RECT anchor{};
            GetWindowRect(GetDlgItem(window_, exportMenu), &anchor);
            const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, anchor.right, anchor.bottom,
                                               0, window_, nullptr);
            DestroyMenu(menu);
            if (choice) click(choice);
            break;
        }
        case saveCsv: exportCsv(); break;
        case saveCalibration:
#ifdef OPENCAP_RECEIVER
            if (const auto samples = receiver_ ? receiver_->calibration(static_cast<Side>(side_)) : std::nullopt) {
                if (const auto saved = exportCalibrationReport(window_, static_cast<Side>(side_), *samples))
                    status_ = *saved ? L"Calibration report saved." : L"Could not save the calibration report.";
                break;
            }
#endif
            status_ = L"Export requires a saved guided calibration for this glove.";
            break;
        case handView:
            if (!openHandView(window_, static_cast<unsigned>(side_))) status_ = L"Could not open the hand viewer.";
            break;
        case dashboard: {
            const auto exe = executableFolder() / "OpenCapDashboard.exe";
            const auto rc = ShellExecuteW(window_, L"open", exe.c_str(), L"--show", exe.parent_path().c_str(), SW_SHOWNORMAL);
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
            bus_.send({static_cast<Side>(side), Action::hapticTest, snapshot_.hands[side].controllerRevision == 2 ? 1 : 0});
    }

    void changeTracker(std::size_t side, bool automatic) {
        auto& hand = prefs_.hands[side];
        if (automatic) {
            hand.tracker = noTracker;
            hand.trackerSerial.fill(0);
            ++hand.trackerSelectionRevision;
        } else {
            const auto tracking = bus_.readTracking();
            if (!tracking || !trackingCurrent(*tracking, now()) || !tracking->count) {
                status_ = L"No SteamVR tracker is available.";
                return;
            }
            const auto count = (std::min<std::size_t>)(tracking->count, tracking->devices.size());
            const auto devices = std::span{tracking->devices}.first(count);
            const auto current = std::ranges::find_if(devices, [&](const TrackerDevice& device) {
                return hand.trackerSerial[0] ? hand.trackerSerial == device.serial : hand.tracker == device.index;
            });
            const auto next = current != devices.end() && std::next(current) != devices.end() ? std::next(current) : devices.begin();
            hand.tracker = noTracker;
            hand.trackerSerial = next->serial;
        }
        if (side_ == side) select(side);
        save();
    }

    void usePreset(std::size_t preset) {
        if (!applyTrackingPreset(prefs_.hands, preset)) return;
        SendDlgItemMessageW(window_, trackingPlan, CB_SETCURSEL, preset, 0);
        select(side_);
        if (saveSettings(folder_, prefs_))
            status_ = std::format(L"Applied {} alignment to both hands.", trackingPresets[preset].name);
        else status_ = L"Alignment was applied but could not be saved.";
    }

    bool sendCalibration(Action action) {
        std::array<Command, handCount> commands{};
        std::size_t count = 0;
        for (unsigned i = 0; i < handCount; ++i)
            if (snapshot_.calibrationGuide.hands & (1u << i))
                commands[count++] = {static_cast<Side>(i), action, quickId_};
        return bus_.send(std::span(commands).first(count));
    }

    void startQuick() {
        auto& guide = snapshot_.calibrationGuide;
        if (guide.hands) return;
        const auto time = now();
        const auto hands = handsReadyForCalibration(snapshot_, time);
        if (!hands) {
            finishQuick(CalibrationResult::failed, L"Connect a glove and finish its joystick calibration.");
            return;
        }
        if (quickId_ == std::numeric_limits<int>::max()) quickId_ = 0;
        ++quickId_;
        guide = {hands, 1};
        if (!sendCalibration(Action::startHand)) {
            finishQuick(CalibrationResult::failed, L"The command queue is busy. Start calibration again.");
            return;
        }
        quickDeadline_ = time + captureTimeout;
        status_.clear();
    }

    void finishQuick(CalibrationResult result, std::wstring message) {
        auto& guide = snapshot_.calibrationGuide;
        guide = {};
        guide.result = result;
        std::copy_n(message.data(), (std::min)(message.size(), guide.message.size() - 1), guide.message.data());
        status_ = std::move(message);
    }

    void stopQuick(std::wstring reason = L"Hand calibration canceled.", CalibrationResult result = CalibrationResult::canceled) {
        if (!sendCalibration(Action::cancelHand)) {
            status_ = L"The command queue is busy. Try canceling calibration again.";
            return;
        }
        finishQuick(result, std::move(reason));
        voice_.say(status_.c_str());
    }

    void guide(Millis time) {
        auto& guide = snapshot_.calibrationGuide;
        if (!guide.hands) return;
        bool ready = true;
        bool sessionOnly = false;
        for (std::size_t i = 0; i < handCount; ++i) {
            if (!(guide.hands & (1u << i))) continue;
            const auto& hand = snapshot_.hands[i];
            if (!isLive(snapshot_, static_cast<Side>(i), time)) {
                stopQuick(L"Glove connection lost. Reconnect and start calibration again.", CalibrationResult::failed);
                return;
            }
            if (hand.calibrationId != quickId_) { ready = false; continue; }
            if (hand.calibrationResult == CalibrationResult::failed) {
                stopQuick(hand.message[0] ? widen(hand.message.data()) : L"Could not record the pose. Restart calibration.",
                          CalibrationResult::failed);
                return;
            }
            const bool saving = guide.phase > captureActions.size();
            if (saving && hand.calibrationResult == CalibrationResult::sessionOnly && !hand.calibrationStep) {
                sessionOnly = true;
                continue;
            }
            const auto expected = saving ? CalibrationResult::saved : CalibrationResult::active;
            if (hand.calibrationResult != expected || hand.calibrationStep != (saving ? 0 : guide.phase)) ready = false;
        }
        if (!ready) {
            if (time >= quickDeadline_)
                stopQuick(L"The glove did not confirm the pose. Restart calibration.", CalibrationResult::failed);
            return;
        }
        if (guide.phase > captureActions.size()) {
            if (sessionOnly)
                finishQuick(CalibrationResult::sessionOnly, L"Calibration applied. A glove profile could not be saved for the next session.");
            else finishQuick(CalibrationResult::saved, L"Hand calibration complete.");
            voice_.say(L"Calibration complete.");
            return;
        }
        if (!guide.captureAt) {
            voice_.say(calibrationPose(guide.phase));
            guide.captureAt = time + calibrationCountdown;
            return;
        }
        if (time < guide.captureAt) return;
        if (!sendCalibration(captureActions[guide.phase - 1])) {
            if (time - guide.captureAt >= captureTimeout)
                stopQuick(L"The command queue is busy. Restart calibration.", CalibrationResult::failed);
            return;
        }
        ++guide.phase;
        quickDeadline_ = time + captureTimeout;
        guide.captureAt = 0;
    }

    void tick() {
#ifdef OPENCAP_RECEIVER
        if (nativeUpdate_.valid()) {
            if (nativeUpdate_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                resumeReceivers();
                EnableWindow(GetDlgItem(window_, firmwareUpdates), TRUE);
                try {
                    nativeUpdate_.get();
                    status_ = nativeWireless_ ? L"The glove accepted the update. Power it on again and check its firmware version."
                                              : L"The device accepted the update. Check the firmware version after it restarts.";
                } catch (const std::exception& error) { status_ = widen(error.what()); }
            } else {
                const auto progress = firmwareProgress_.load();
                status_ = progress == 100 ? L"Firmware transfer accepted. Keep the device connected until the update completes."
                    : std::format(L"Firmware update {}%. Keep the device connected.", progress);
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
#endif
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
            if (command->action == Action::togglePublishing || command->action == Action::toggleHaptics) {
                auto& hand = prefs_.hands[s];
                auto& enabled = command->action == Action::togglePublishing ? hand.enabled : hand.haptics;
                enabled = !enabled;
                if (side_ == s) select(s);
                save();
                continue;
            }
            if (command->action == Action::cycleTracker || command->action == Action::autoTracker) {
                changeTracker(s, command->action == Action::autoTracker);
                continue;
            }
            if (command->action == Action::channel) {
                const auto other = 1 - s;
                if (snapshot_.hands[other].receiver && (command->value == snapshot_.hands[other].channel ||
                    command->value == snapshot_.hands[other].pendingChannel)) {
                    status_ = L"Choose a different channel from the other glove.";
                    continue;
                }
            }
            if (command->action == Action::applyPreset) {
                usePreset(static_cast<std::size_t>(command->value));
                continue;
            }
            if (command->action == Action::guidedCalibration) {
                startQuick();
                continue;
            }
            if (command->action == Action::cancelHand && !command->value &&
                snapshot_.calibrationGuide.hands) {
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
                auto& result = demoResults_[s];
                if (isHandCalibration(command->action) && command->action != Action::startHand &&
                    command->value != demoCalibrationIds_[s]) continue;
                switch (command->action) {
                case Action::startHand:
                    if (!step) {
                        demoCalibrationIds_[s] = command->value;
                        result = CalibrationResult::active;
                        step = 1;
                    }
                    break;
                case Action::fist: if (step == 1) step = 2; break;
                case Action::together: if (step == 2) step = 3; break;
                case Action::spread: if (step == 3) { step = 0; result = CalibrationResult::saved; } break;
                case Action::cancelHand:
                    if (step && step <= 3) { step = 0; result = CalibrationResult::canceled; }
                    break;
                case Action::joystickCenter: if (!step || step == 4) step = 4; break;
                case Action::joystickFinish: if (step == 4) step = 0; break;
                case Action::pairStart: demoPairing_[s] = true; break;
                case Action::pairStop: demoPairing_[s] = false; break;
                case Action::pairBoth: demoPairing_.fill(true); break;
                case Action::pairBothStop: demoPairing_.fill(false); break;
                case Action::channel:
                    if (command->value >= 0 && command->value <= 100) demoChannels_[s] = command->value;
                    break;
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
        if (receiver_) {
            receiver_->setTriggerFeedback(prefs_.hands);
            snapshot_.hands = receiver_->read();
        }
        if (prefs_.recordCalibration && !calibrationLog_.active()) {
            const auto error = calibrationLog_.takeError();
            configureRecording(false);
            status_ = error.empty() ? L"Calibration recording stopped." : widen(error);
        }
#endif
        if (demo_) {
            for (std::size_t s = 0; s < handCount; ++s) {
                snapshot_.hands[s] = demoHand(static_cast<Side>(s), time);
                snapshot_.hands[s].calibrationStep = demoSteps_[s];
                snapshot_.hands[s].calibrationId = demoCalibrationIds_[s];
                snapshot_.hands[s].calibrationResult = demoResults_[s];
                snapshot_.hands[s].channel = demoChannels_[s];
                snapshot_.hands[s].pairing = demoPairing_[s];
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
        for (std::size_t s = 0; s < handCount; ++s) {
            const auto count = snapshot_.hands[s].calibrationRequests;
            if (count > lastRequests_[s] && snapshot_.hands[s].calibrationStep == 0)
                startQuick();
            lastRequests_[s] = count;
        }
        guide(snapshot_.heartbeat);
        bus_.publish(snapshot_);
        stream_.send(snapshot_);
        const auto& selected = snapshot_.hands[side_];
        const bool gloveLive = isLive(snapshot_, static_cast<Side>(side_), snapshot_.heartbeat);
        const bool editable = gloveLive && selected.calibrated;
        if (editable != handEditable_) {
            handEditable_ = editable;
            EnableWindow(GetDlgItem(window_, jointAdjustment), editable);
            EnableWindow(GetDlgItem(window_, handResponse), editable && !demo_);
        }
        const auto revision = selected.controllerRevision;
        if (lastControllerRevision_ != revision || lastVibrationAvailable_ != gloveLive) {
            lastControllerRevision_ = revision;
            lastVibrationAvailable_ = gloveLive;
            SetDlgItemTextW(window_, vibrate, revision == 2 ? L"Test low vibration" : L"Test motor 1");
            SetDlgItemTextW(window_, vibrateSecond, revision == 2 ? L"Test high vibration" : L"Test motor 2");
            EnableWindow(GetDlgItem(window_, vibrate), gloveLive);
            EnableWindow(GetDlgItem(window_, vibrateSecond), gloveLive);
        }
        const auto stage = selected.calibrationStep;
        const bool guiding = snapshot_.calibrationGuide.hands != 0;
        const bool calibrationReady = handsReadyForCalibration(snapshot_, snapshot_.heartbeat) != 0;
        if (lastCalibrationReady_ != calibrationReady) {
            lastCalibrationReady_ = calibrationReady;
            EnableWindow(GetDlgItem(window_, nextPose), calibrationReady);
        }
        if (lastStage_ != stage || lastGuiding_ != guiding || lastCalibrationLive_ != gloveLive) {
            lastStage_ = stage;
            lastGuiding_ = guiding;
            lastCalibrationLive_ = gloveLive;
            SetDlgItemTextW(window_, nextPose, guiding ? L"Calibration running…" : L"Guided calibration");
            EnableWindow(GetDlgItem(window_, cancelPose), guiding || (stage >= 1 && stage <= 3));
            EnableWindow(GetDlgItem(window_, centerStick), gloveLive && !guiding && (stage == 0 || stage == 4));
            EnableWindow(GetDlgItem(window_, finishStick), gloveLive && stage == 4);
            relayout();
            if (stage == 4 && GetFocus() == GetDlgItem(window_, centerStick))
                SetFocus(GetDlgItem(window_, finishStick));
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

    void openHandResponse() {
#ifdef OPENCAP_RECEIVER
        const auto side = static_cast<Side>(side_);
        if (receiver_ && isLive(snapshot_, side, now()) && snapshot_.hands[side_].calibrated) {
            if (const auto curves = receiver_->handCurves(side)) {
                if (const auto edit = editHandCurve(window_, *curves)) {
                    if (!isLive(snapshot_, side, now()) || !bus_.send(Command{.side = side, .action = Action::setHandCurve,
                            .value = static_cast<int>(edit->channel), .curve = edit->curve}))
                        status_ = L"Connect the glove and try saving the hand response again.";
                }
                return;
            }
        }
#endif
        status_ = L"Connect this glove and move your fingers before editing its response.";
    }

#ifdef OPENCAP_RECEIVER
    void firmwareMenu() {
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, 1, L"Update via USB…");
        AppendMenuW(menu, MF_STRING, 2, L"Update a wireless glove…");
        RECT button{};
        GetWindowRect(GetDlgItem(window_, firmwareUpdates), &button);
        const auto action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                           button.left, button.bottom, 0, window_, nullptr);
        DestroyMenu(menu);
        if (action) nativeFirmwareUpdate(action == 2);
    }

    void pauseReceivers() {
        if (snapshot_.calibrationGuide.hands) stopQuick();
        receiver_.reset();
        snapshot_.hands = {};
        bus_.publish(snapshot_);
        stream_.send(snapshot_);
    }

    void nativeFirmwareUpdate(bool radio) {
        if (nativeUpdate_.valid()) return;
        wchar_t chosen[MAX_PATH]{};
        OPENFILENAMEW request{};
        request.lStructSize = sizeof(request);
        request.hwndOwner = window_;
        request.lpstrFilter = L"UDCAP firmware image (*.bin)\0*.bin\0\0";
        request.lpstrFile = chosen;
        request.nMaxFile = MAX_PATH;
        request.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&request)) return;
        try {
            auto image = readUpdateImage(chosen);
            std::optional<WirelessFirmwareTarget> wireless;
            if (radio) {
                if (image.target == FirmwareTarget::receiver)
                    throw std::runtime_error("Use the USB update for a receiver image.");
                const auto side = image.target == FirmwareTarget::leftGlove ? Side::left : Side::right;
                if (receiver_) wireless = receiver_->wirelessFirmwareTarget(side);
                if (!wireless) throw std::runtime_error("Power on the glove that matches the image and wait for a wireless connection.");
            }
            const auto device = image.target == FirmwareTarget::receiver ? L"single-hand receiver" :
                                image.target == FirmwareTarget::leftGlove ? L"left glove" : L"right glove";
            const auto& v = image.version;
            const auto prompt = std::format(L"Update the {} to {}.{}.{}? {}", device, +v.major, +v.minor, v.revision,
                radio ? L"Keep the glove powered near its receiver until the update finishes."
                      : L"Connect one device of this type via USB and keep it connected until the update finishes.");
            if (MessageBoxW(window_, prompt.c_str(), L"Firmware update", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            pauseReceivers();
            firmwareProgress_.store(0);
            nativeWireless_ = radio;
            nativeUpdate_ = std::async(std::launch::async, [this, image = std::move(image), wireless = std::move(wireless)] {
                const auto progress = [this](unsigned completed) { firmwareProgress_.store(completed); };
                if (wireless) updateWirelessGlove(image, *wireless, progress);
                else updateWiredDevice(image, progress);
            });
            EnableWindow(GetDlgItem(window_, firmwareUpdates), FALSE);
        } catch (const std::exception& error) {
            if (!receiver_) resumeReceivers();
            status_ = widen(error.what());
        }
    }
#endif

    void resumeReceivers() {
        status_.clear();
#ifdef OPENCAP_RECEIVER
        if (folder_.empty()) return;
        try {
            auto vendor = readUdcapPreferences(findUdcapSources(folder_.parent_path()));
            receiver_ = std::make_unique<Receiver>(folder_ / "Gloves", std::move(vendor.receiver),
                                                   prefs_.udcapImport, prefs_.automaticHandCalibration, calibrationLog_);
        } catch (const std::exception& error) {
            status_ = widen(error.what());
        }
#endif
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
    unsigned lastControllerRevision_{UINT32_MAX};
    bool lastVibrationAvailable_{}, lastGuiding_{}, lastCalibrationLive_{}, lastCalibrationReady_{};
    std::optional<bool> handEditable_;
#ifdef OPENCAP_RECEIVER
    CalibrationLog calibrationLog_;
    std::unique_ptr<Receiver> receiver_;
    std::atomic<unsigned> firmwareProgress_{};
    bool nativeWireless_{};
    std::future<void> nativeUpdate_;
#endif
    HWND window_{};
    DesktopPage page_{DesktopPage::gloves};
    DesktopLayout layout_;
    unsigned dpi_{96};
    int scroll_{}, clientHeight_{}, wheelDelta_{};
    bool layingOut_{};
    HFONT font_{}, controlFont_{}, section_{};
    HBRUSH brush_{};
    std::size_t side_{};
    unsigned lastStage_{UINT32_MAX};
    Millis lastPaint_{};
    std::array<unsigned, handCount> demoSteps_{}, lastRequests_{};
    std::array<int, handCount> demoCalibrationIds_{};
    std::array<CalibrationResult, handCount> demoResults_{};
    std::array<int, handCount> demoChannels_{2, 7};
    std::array<bool, handCount> demoPairing_{};
    std::array<std::array<Vec3, jointCount>, handCount> demoOffsets_{};
    std::array<ButtonRanges, handCount> demoRanges_{defaultButtonRanges, defaultButtonRanges};
    std::array<StickHold, handCount> stickHolds_{};
    std::wstring status_;
    bool bothPairing_{};
    int quickId_{};
    Millis quickDeadline_{};
};
}

int opencap::runDesktop(HINSTANCE instance, int show, bool demo, const std::wstring& session, bool isolated, HANDLE steamOverlay) {
    Desktop app(demo, session, isolated, steamOverlay);
    return app.run(instance, show);
}
