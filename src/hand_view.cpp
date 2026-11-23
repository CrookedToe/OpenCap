#include "hand_view.hpp"
#include "bridge.hpp"
#include "vr_skeleton.hpp"

#include <windowsx.h>
#include <algorithm>
#include <array>
#include <format>
#include <new>

namespace opencap {
namespace {
constexpr wchar_t windowClass[] = L"OpenCapHandView";
constexpr COLORREF background = RGB(17, 27, 40);
constexpr COLORREF foreground = RGB(234, 243, 248);
constexpr COLORREF active = RGB(96, 224, 201);
constexpr COLORREF inactive = RGB(128, 146, 162);
constexpr INT_PTR modeButton = 10, resetButton = 11;
constexpr std::array<const wchar_t*, 5> fingers{L"Thumb", L"Index", L"Middle", L"Ring", L"Pinky"};
constexpr std::array gradeColors{RGB(255, 144, 137), RGB(232, 197, 116), RGB(171, 224, 141), RGB(102, 205, 155)};

struct View {
    Bus bus;
    Quaternion camera{};
    float pitch{17}, yaw{-15};
    Side side{};
    bool dataMode{};
    bool orbiting{};
    POINT previous{};

    void reset() {
        pitch = 17;
        yaw = -15;
        camera = eulerDegrees({pitch, yaw, 0});
    }

    void orbit(int dx, int dy) {
        pitch = std::clamp(pitch + dy * .4f, -80.0f, 80.0f);
        yaw += dx * .4f;
        camera = eulerDegrees({pitch, yaw, 0});
    }

    POINT project(Vec3 point, int x0, int y0, float scale) const {
        const auto rotated = rotate(camera, point);
        return {static_cast<LONG>(x0 + scale * rotated.x), static_cast<LONG>(y0 - scale * rotated.y)};
    }

    void drawHand(HDC dc, const RECT& size, const Hand& hand, bool live) const {
        const auto chains = makeHandPreview(side, live ? hand : Hand{});
        const auto scale = static_cast<float>((std::min)(size.right, size.bottom - 88)) * 3.6f;
        const auto x0 = size.right / 2;
        const auto y0 = static_cast<int>((size.bottom + 88) / 2 + .08f * scale);
        const auto pen = CreatePen(PS_SOLID, 4, live ? active : inactive);
        const auto fill = CreateSolidBrush(live ? active : inactive);
        const auto previousPen = SelectObject(dc, pen);
        const auto previousFill = SelectObject(dc, fill);
        for (const auto& chain : chains) {
            auto p = project({}, x0, y0, scale);
            for (const auto& joint : chain) {
                const auto next = project(joint, x0, y0, scale);
                MoveToEx(dc, p.x, p.y, nullptr);
                LineTo(dc, next.x, next.y);
                Ellipse(dc, next.x - 3, next.y - 3, next.x + 3, next.y + 3);
                p = next;
            }
        }
        SelectObject(dc, previousFill);
        SelectObject(dc, previousPen);
        DeleteObject(fill);
        DeleteObject(pen);
    }

    void paint(HWND window) const {
        PAINTSTRUCT ps{};
        const auto target = BeginPaint(window, &ps);
        RECT size{};
        GetClientRect(window, &size);
        const auto dc = CreateCompatibleDC(target);
        const auto bitmap = CreateCompatibleBitmap(target, size.right, size.bottom);
        const auto savedBitmap = SelectObject(dc, bitmap);
        const auto fill = CreateSolidBrush(background);
        FillRect(dc, &size, fill);
        DeleteObject(fill);
        SetTextColor(dc, foreground);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        const auto sample = bus.read().value_or(Snapshot{});
        const auto& hand = sample.hands[static_cast<std::size_t>(side)];
        const auto live = isLive(sample, side, now());
        auto title = live ? L"Live hand data" : L"No recent glove data";
        if (!dataMode && live) title = L"Right-drag to orbit the hand";
        TextOutW(dc, 20, 63, title, lstrlenW(title));
        if (dataMode) {
            for (std::size_t f = 0; f < fingers.size(); ++f) {
                const auto q = hand.joints[3*f].rotation;
                const auto line = std::format(L"{:<8}    Curl {:5.0f}%    Proximal XYZW  {:>+.3f} {:>+.3f} {:>+.3f} {:>+.3f}",
                   fingers[f], 100*hand.curl[f], q.x, q.y, q.z, q.w);
                TextOutW(dc, 20, 104 + static_cast<int>(f)*35, line.c_str(), static_cast<int>(line.size()));
            }
            if (live && hand.quality.measured) {
                const std::wstring_view label = L"Calibration movement in raw sensor counts";
                TextOutW(dc, 20, 300, label.data(), static_cast<int>(label.size()));
                for (std::size_t i = 0; i < hand.quality.counts.size(); ++i) {
                    const auto count = hand.quality.counts[i];
                    const auto grade = calibrationGrade(i, count);
                    const auto value = std::format(L"{}   {}   {}", i+4, count, widen(calibrationGradeName(grade)));
                    SetTextColor(dc, gradeColors[static_cast<std::size_t>(grade)]);
                    TextOutW(dc, 20 + static_cast<int>(i % 3)*150, 327 + static_cast<int>(i / 3)*24,
                             value.c_str(), static_cast<int>(value.size()));
                }
            }
        } else {
            drawHand(dc, size, hand, live);
        }
        BitBlt(target, 0, 0, size.right, size.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, savedBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &ps);
    }
};

LRESULT CALLBACK handle(HWND window, UINT event, WPARAM wParam, LPARAM lParam) {
    const auto view = reinterpret_cast<View*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (event) {
    case WM_CREATE: {
        auto state = new (std::nothrow) View;
        if (!state) return -1;
        const auto init = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state->side = static_cast<Side>(reinterpret_cast<UINT_PTR>(init->lpCreateParams));
        state->reset();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        CreateWindowW(L"BUTTON", L"Show data", WS_CHILD | WS_VISIBLE, 20, 17, 110, 32, window,
                      reinterpret_cast<HMENU>(modeButton), init->hInstance, nullptr);
        CreateWindowW(L"BUTTON", L"Reset camera", WS_CHILD | WS_VISIBLE, 142, 17, 120, 32, window,
                      reinterpret_cast<HMENU>(resetButton), init->hInstance, nullptr);
        SetTimer(window, 1, 40, nullptr);
        return 0;
    }
    case WM_TIMER: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_COMMAND:
        if (!view) return 0;
        if (LOWORD(wParam) == modeButton) {
            view->dataMode = !view->dataMode;
            SetDlgItemTextW(window, modeButton, view->dataMode ? L"Show hand" : L"Show data");
        } else if (LOWORD(wParam) == resetButton) view->reset();
        return 0;
    case WM_RBUTTONDOWN:
        if (!view || view->dataMode) return 0;
        view->orbiting = true;
        view->previous = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP: ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: if (view) view->orbiting = false; return 0;
    case WM_MOUSEMOVE:
        if (view && view->orbiting) {
            const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            view->orbit(position.x - view->previous.x, position.y - view->previous.y);
            view->previous = position;
        }
        return 0;
    case WM_PAINT: if (view) { view->paint(window); return 0; } break;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: KillTimer(window, 1); return 0;
    case WM_NCDESTROY:
        delete view;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, event, wParam, lParam);
}
}

bool openHandView(HWND owner, unsigned side) {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = handle;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = windowClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    const auto window = CreateWindowExW(0, windowClass, side ? L"OpenCap — Right hand" : L"OpenCap — Left hand",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 680, 530,
            owner, nullptr, instance, reinterpret_cast<void*>(static_cast<UINT_PTR>(side)));
    if (!window) return false;
    ShowWindow(window, SW_SHOW);
    return true;
}
}
