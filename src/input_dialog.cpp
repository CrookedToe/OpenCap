#include "input_dialog.hpp"
#include "bridge.hpp"

#include <commctrl.h>
#include <algorithm>

#include <array>
#include <format>
#include <locale>
#include <sstream>

namespace opencap {
namespace {
constexpr wchar_t className[] = L"OpenCapInputMapping";
constexpr int sourceBase = 100;
constexpr int saveButton = 120;
constexpr int cancelButton = 121;
constexpr int enableBase = 130, thresholdBase = 140;
constexpr int meterBase = 150, controllerState = 153;
constexpr std::array outputs{L"A button", L"B button", L"System", L"Joystick click"};
constexpr std::array sources{L"A button", L"B button", L"System button", L"Joystick click",
                             L"Power button", L"Trigger", L"Grip", L"Trackpad touch", L"Disabled"};

struct Dialog {
    ControllerEdit edit{};
    const Hand& sample;
    bool live{};
    bool saved{};
    bool closed{};
};
struct JointDialog {
    const std::array<Vec3, jointCount>& offsets;
    std::optional<JointEdit> edit;
    bool closed{};
};
struct CurveDialog {
    const HandCurves& curves;
    std::optional<CurveEdit> edit;
    bool closed{};
};

HWND item(HWND window, const wchar_t* type, const wchar_t* label, DWORD style,
          int x, int y, int width, int height, int id) {
    const auto control = CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

template<class Number>
bool readNumber(HWND window, int field, Number& number) {
    wchar_t value[32]{};
    GetDlgItemTextW(window, field, value, 32);
    std::wistringstream input(value);
    input.imbue(std::locale::classic());
    return (input >> number) && (!(input >> std::ws) || input.eof());
}

void refreshController(HWND window, const Hand& sample) {
    const auto time = now();
    const bool live = sample.linked && sample.lastFrame && time >= sample.lastFrame &&
        time - sample.lastFrame <= sampleTimeout;
    const std::array values{sample.trigger, sample.grip, sample.pad};
    for (std::size_t i = 0; i < values.size(); ++i)
        SendDlgItemMessageW(window, meterBase + static_cast<int>(i), PBM_SETPOS,
            live ? static_cast<WPARAM>(100 * std::clamp(values[i], 0.0f, 1.0f)) : 0, 0);
    std::wstring controls;
    if (live && sample.controllerRevision) controls = std::format(L"Controller {}.0", sample.controllerRevision);
    if (!live) controls = L"Glove offline";
    if (live) for (const auto& [button, label] : std::array{
         std::pair{Button::a, L"A"}, std::pair{Button::b, L"B"},
         std::pair{Button::system, L"System"}, std::pair{Button::stick, L"Stick"}}) {
        if (sample.buttons & bit(button)) {
            if (!controls.empty()) controls += L"   ";
            controls += label;
        }
    }
    SetDlgItemTextW(window, controllerState, controls.c_str());
}

LRESULT CALLBACK handle(HWND window, UINT event, WPARAM wParam, LPARAM lParam) {
    if (event == WM_NCCREATE) {
        const auto data = reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    const auto dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (event) {
    case WM_CREATE:
        item(window, L"STATIC", L"SteamVR output", 0, 20, 20, 116, 24, 0);
        item(window, L"STATIC", L"Glove input", 0, 140, 20, 210, 24, 0);
        for (std::size_t row = 0; row < outputs.size(); ++row) {
            const auto id = sourceBase + static_cast<int>(row);
            const auto y = 46 + static_cast<int>(row)*35;
            item(window, L"STATIC", outputs[row], SS_CENTERIMAGE, 20, y, 110, 28, 0);
            const auto combo = item(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                    140, y, 205, 260, id);
            for (const auto source : sources) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(source));
            SendMessageW(combo, CB_SETCURSEL, dialog->edit.mapping[row], 0);
        }
        item(window, L"STATIC", L"Touch %", 0, 140, 198, 90, 20, 0);
        item(window, L"STATIC", L"Click %", 0, 250, 198, 90, 20, 0);
        {
            constexpr std::array names{L"Trigger", L"Grip", L"Trackpad", L"Thumb tracking"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto y = 222 + static_cast<int>(i) * 34;
                const auto id = enableBase + static_cast<int>(i);
                item(window, L"BUTTON", names[i], WS_TABSTOP | BS_AUTOCHECKBOX, 20, y, 110, 26, id);
                CheckDlgButton(window, id, dialog->edit.enabled[i] ? BST_CHECKED : BST_UNCHECKED);
                if (i == dialog->edit.ranges.size()) continue;
                const auto r = dialog->edit.ranges[i];
                const std::array percentages{100*r.touch, 100*r.click};
                for (int j = 0; j < 2; ++j) {
                    const auto field = item(window, L"EDIT", std::format(L"{:.2f}", percentages[j]).c_str(),
                        WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 140 + 110*j, y, 95, 26,
                        thresholdBase + static_cast<int>(2*i) + j);
                    EnableWindow(field, dialog->live);
                }
            }
        }
        item(window, L"STATIC", L"", SS_CENTERIMAGE, 20, 359, 324, 22, controllerState);
        {
            constexpr std::array names{L"Trigger input", L"Grip input", L"Trackpad input"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto y = 387 + static_cast<int>(i) * 25;
                item(window, L"STATIC", names[i], SS_CENTERIMAGE, 20, y, 112, 18, 0);
                item(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, 140, y + 1, 205, 16,
                     meterBase + static_cast<int>(i));
            }
        }
        refreshController(window, dialog->sample);
        SetTimer(window, 1, 50, nullptr);
        item(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 140, 474, 95, 28, cancelButton);
        item(window, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 474, 95, 28, saveButton);
        return 0;
    case WM_TIMER: refreshController(window, dialog->sample); return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == saveButton) {
            for (std::size_t row = 0; row < outputs.size(); ++row) {
                const auto selected = SendDlgItemMessageW(window, sourceBase + static_cast<int>(row), CB_GETCURSEL, 0, 0);
                if (selected < 0 || selected >= static_cast<LRESULT>(sources.size())) return 0;
                dialog->edit.mapping[row] = static_cast<unsigned>(selected);
            }
            for (std::size_t i = 0; i < dialog->edit.enabled.size(); ++i)
                dialog->edit.enabled[i] = IsDlgButtonChecked(window, enableBase + static_cast<int>(i)) == BST_CHECKED;
            if (dialog->live) for (std::size_t i = 0; i < dialog->edit.ranges.size(); ++i) {
                ButtonRange r;
                const auto id = thresholdBase + static_cast<int>(2*i);
                if (!readNumber(window, id, r.touch) || !readNumber(window, id+1, r.click)) return 0;
                r = {r.touch / 100, r.click / 100};
                if (!validButtonRange(r)) {
                    MessageBoxW(window, L"Enter a touch threshold and click threshold between 0 and 100, in ascending order.",
                                L"OpenCap", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                dialog->edit.ranges[i] = r;
            }
            dialog->saved = true;
            DestroyWindow(window);
        }
        if (LOWORD(wParam) == cancelButton) DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        KillTimer(window, 1);
        if (dialog) dialog->closed = true;
        break;
    }
    return DefWindowProcW(window, event, wParam, lParam);
}

constexpr int jointBox = 100, firstAxis = 101;

void showOffsets(HWND window, const JointDialog& dialog) {
    const auto selected = static_cast<std::size_t>(SendDlgItemMessageW(window, jointBox, CB_GETCURSEL, 0, 0));
    if (selected >= dialog.offsets.size()) return;
    const auto value = dialog.offsets[selected];
    const std::array coordinates{value.x, value.y, value.z};
    for (std::size_t i = 0; i < coordinates.size(); ++i)
        SetDlgItemTextW(window, firstAxis + static_cast<int>(i), std::format(L"{:.2f}", coordinates[i]).c_str());
}

LRESULT CALLBACK jointHandler(HWND window, UINT event, WPARAM wParam, LPARAM lParam) {
    if (event == WM_NCCREATE) {
        const auto data = reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    const auto dialog = reinterpret_cast<JointDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (event) {
    case WM_CREATE: {
        item(window, L"STATIC", L"Joint", 0, 20, 20, 104, 24, 0);
        const auto box = item(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                              20, 44, 324, 280, jointBox);
        for (const auto finger : {L"Thumb", L"Index", L"Middle", L"Ring", L"Pinky"}) {
            for (const auto segment : {L"proximal", L"intermediate", L"distal"}) {
                const auto name = std::format(L"{} {}", finger, segment);
                SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
        }
        SendMessageW(box, CB_SETCURSEL, 0, 0);
        for (int i = 0; i < 3; ++i) {
            const auto x = 20 + i * 110;
            item(window, L"STATIC", std::format(L"{} (degrees)", L"XYZ"[i]).c_str(), 0, x, 89, 95, 24, 0);
            item(window, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, x, 113, 102, 28, firstAxis + i);
        }
        item(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 140, 172, 95, 28, cancelButton);
        item(window, L"BUTTON", L"Apply", WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 172, 95, 28, saveButton);
        showOffsets(window, *dialog);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == jointBox && HIWORD(wParam) == CBN_SELCHANGE) showOffsets(window, *dialog);
        if (LOWORD(wParam) == cancelButton) DestroyWindow(window);
        if (LOWORD(wParam) == saveButton) {
            const auto chosen = SendDlgItemMessageW(window, jointBox, CB_GETCURSEL, 0, 0);
            if (chosen < 0 || chosen >= static_cast<LRESULT>(jointCount)) return 0;
            JointEdit edit{static_cast<int>(chosen)};
            const std::array fields{&edit.offset.x, &edit.offset.y, &edit.offset.z};
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (!readNumber(window, firstAxis + static_cast<int>(i), *fields[i])) {
                    MessageBoxW(window, L"Enter a number for each axis.", L"OpenCap", MB_OK);
                    return 0;
                }
            }
            if (!validJointOffset(edit.offset)) {
                MessageBoxW(window, L"Enter offsets from −45 to 45 degrees.", L"OpenCap", MB_OK);
                return 0;
            }
            dialog->edit = edit;
            DestroyWindow(window);
        }
        return 0;
    case WM_NCDESTROY: if (dialog) dialog->closed = true; break;
    }
    return DefWindowProcW(window, event, wParam, lParam);
}

constexpr int curveChoice = 100, firstCurveField = 101;
constexpr std::array curveNames{
    L"Thumb distal bend", L"Thumb intermediate bend", L"Thumb proximal bend", L"Thumb splay",
    L"Index distal bend", L"Index intermediate bend", L"Index proximal bend", L"Index splay",
    L"Middle distal bend", L"Middle intermediate bend", L"Middle proximal bend", L"Middle splay",
    L"Ring distal bend", L"Ring intermediate bend", L"Ring proximal bend", L"Ring splay",
    L"Pinky distal bend", L"Pinky intermediate bend", L"Pinky proximal bend", L"Pinky splay", L"Thumb roll"
};

void showCurve(HWND window, const CurveDialog& dialog) {
    const auto channel = static_cast<std::size_t>(SendDlgItemMessageW(window, curveChoice, CB_GETCURSEL, 0, 0));
    if (channel >= dialog.curves.size()) return;
    const auto& curve = dialog.curves[channel];
    const std::array values{curve.min, curve.max, curve.balance, curve.offset};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto field = firstCurveField + static_cast<int>(i);
        SetDlgItemTextW(window, field, std::format(L"{:.4f}", values[i]).c_str());
        EnableWindow(GetDlgItem(window, field), channel != 11 || i == 3);
    }
}

LRESULT CALLBACK curveHandler(HWND window, UINT event, WPARAM wParam, LPARAM lParam) {
    if (event == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    const auto dialog = reinterpret_cast<CurveDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (event) {
    case WM_CREATE: {
        item(window, L"STATIC", L"Finger channel", 0, 20, 20, 180, 22, 0);
        const auto combo = item(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                20, 45, 324, 252, curveChoice);
        for (const auto name : curveNames) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        constexpr std::array fields{L"Minimum angle (°)", L"Maximum angle (°)", L"Balance (0 to 1)", L"Offset (°)"};
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto x = 20 + static_cast<int>(i % 2) * 168;
            const auto y = 88 + static_cast<int>(i / 2) * 57;
            item(window, L"STATIC", fields[i], 0, x, y, 150, 22, 0);
            item(window, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, x, y + 22, 155, 26,
                 firstCurveField + static_cast<int>(i));
        }
        item(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 140, 205, 95, 28, cancelButton);
        item(window, L"BUTTON", L"Apply", WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 205, 95, 28, saveButton);
        showCurve(window, *dialog);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == curveChoice && HIWORD(wParam) == CBN_SELCHANGE) showCurve(window, *dialog);
        if (LOWORD(wParam) == cancelButton) DestroyWindow(window);
        if (LOWORD(wParam) == saveButton) {
            CurveEdit edit{static_cast<std::size_t>(SendDlgItemMessageW(window, curveChoice, CB_GETCURSEL, 0, 0))};
            auto& [min, max, balance, offset] = edit.curve;
            if (!readNumber(window, firstCurveField, min) || !readNumber(window, firstCurveField + 1, max) ||
                !readNumber(window, firstCurveField + 2, balance) || !readNumber(window, firstCurveField + 3, offset) ||
                !validHandCurve(edit.channel, edit.curve)) {
                MessageBoxW(window, L"Use angles between −180° and 180° in ascending order, "
                            L"a balance from 0 to 1, and an offset between −45° and 45°.",
                            L"Hand response", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            dialog->edit = edit;
            DestroyWindow(window);
        }
        return 0;
    case WM_NCDESTROY: if (dialog) dialog->closed = true; break;
    }
    return DefWindowProcW(window, event, wParam, lParam);
}

void showDialog(HWND owner, const wchar_t* name, const wchar_t* title,
                WNDPROC procedure, int height, void* state, const bool& closed) {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cls.lpszClassName = name;
    cls.lpfnWndProc = procedure;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    RECT bounds{};
    GetWindowRect(owner, &bounds);
    const auto window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, name,
        title, WS_POPUP | WS_CAPTION | WS_SYSMENU, bounds.left + 180, bounds.top + 140,
        380, height, owner, nullptr, cls.hInstance, state);
    if (!window) return;
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (!closed) {
        const auto status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            DestroyWindow(window);
            break;
        }
        if (message.message == WM_KEYDOWN && message.hwnd == window && message.wParam == VK_ESCAPE)
            DestroyWindow(window);
        else if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}
}

std::optional<ControllerEdit> editInputs(HWND owner, const HandSettings& settings, const Hand& hand, bool live) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    Dialog dialog{{settings.buttonMap, {settings.triggerEnabled, settings.gripEnabled, settings.padEnabled, settings.thumbEnabled},
                   hand.buttonRanges}, hand, live};
    showDialog(owner, className, L"OpenCap controller settings", handle, 548, &dialog, dialog.closed);
    if (dialog.saved) return dialog.edit;
    return std::nullopt;
}

std::optional<JointEdit> editJoint(HWND owner, const std::array<Vec3, jointCount>& offsets) {
    JointDialog dialog{offsets};
    showDialog(owner, L"OpenCapJointCorrection", L"OpenCap joint correction", jointHandler, 251, &dialog, dialog.closed);
    return dialog.edit;
}

std::optional<CurveEdit> editHandCurve(HWND owner, const HandCurves& curves) {
    CurveDialog dialog{curves};
    showDialog(owner, L"OpenCapHandResponse", L"OpenCap hand response", curveHandler, 277, &dialog, dialog.closed);
    return dialog.edit;
}
}
