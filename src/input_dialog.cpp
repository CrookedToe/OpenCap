#include "input_dialog.hpp"

#include <array>

namespace opencap {
namespace {
constexpr wchar_t className[] = L"OpenCapInputMapping";
constexpr int sourceBase = 100;
constexpr int saveButton = 120;
constexpr int cancelButton = 121;
constexpr std::array outputs{L"A button", L"B button", L"System", L"Joystick click"};
constexpr std::array sources{L"A button", L"B button", L"System button", L"Joystick click",
                             L"Power button", L"Trigger", L"Grip", L"Trackpad touch", L"Disabled"};

struct Dialog {
    ButtonMap mapping{};
    bool saved{};
    bool closed{};
};

HWND item(HWND window, const wchar_t* type, const wchar_t* label, DWORD style,
          int x, int y, int width, int height, int id) {
    const auto control = CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
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
            SendMessageW(combo, CB_SETCURSEL, dialog->mapping[row], 0);
        }
        item(window, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 140, 206, 95, 28, cancelButton);
        item(window, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 206, 95, 28, saveButton);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == saveButton) {
            for (std::size_t row = 0; row < outputs.size(); ++row) {
                const auto selected = SendDlgItemMessageW(window, sourceBase + static_cast<int>(row), CB_GETCURSEL, 0, 0);
                if (selected < 0 || selected >= static_cast<LRESULT>(sources.size())) return 0;
                dialog->mapping[row] = static_cast<unsigned>(selected);
            }
            dialog->saved = true;
            DestroyWindow(window);
        }
        if (LOWORD(wParam) == cancelButton) DestroyWindow(window);
        return 0;
    case WM_NCDESTROY: if (dialog) dialog->closed = true; break;
    }
    return DefWindowProcW(window, event, wParam, lParam);
}
}

std::optional<ButtonMap> editInputs(HWND owner, ButtonMap mapping) {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cls.lpszClassName = className;
    cls.lpfnWndProc = handle;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return std::nullopt;
    Dialog dialog{mapping};
    RECT bounds{};
    GetWindowRect(owner, &bounds);
    const auto window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, className,
        L"OpenCap controller mapping", WS_POPUP | WS_CAPTION | WS_SYSMENU, bounds.left + 180, bounds.top + 140,
        380, 285, owner, nullptr, cls.hInstance, &dialog);
    if (!window) return std::nullopt;
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (!dialog.closed) {
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
    if (dialog.saved) return dialog.mapping;
    return std::nullopt;
}
}
