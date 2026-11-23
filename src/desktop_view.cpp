#include "desktop_view.hpp"
#include "bridge.hpp"
#include "firmware_image.hpp"

#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <locale>
#include <cmath>
#include <format>

namespace opencap {
namespace {
constexpr COLORREF ink = RGB(22, 36, 48);
constexpr COLORREF accent = RGB(0, 111, 113);
constexpr COLORREF secondary = RGB(73, 94, 109);

void text(HDC dc, int x, int y, std::wstring_view value, COLORREF color = ink) {
    SetTextColor(dc, color);
    TextOutW(dc, x, y, value.data(), static_cast<int>(value.size()));
}

void card(HDC dc, RECT bounds, COLORREF color = RGB(255, 255, 255)) {
    const auto fill = CreateSolidBrush(color);
    const auto previous = SelectObject(dc, fill);
    const auto pen = CreatePen(PS_SOLID, 1, RGB(220, 230, 235));
    const auto previousPen = SelectObject(dc, pen);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, 16, 16);
    SelectObject(dc, previousPen);
    SelectObject(dc, previous);
    DeleteObject(pen);
    DeleteObject(fill);
}

void heading(HDC dc, HFONT font, int x, int y, std::wstring_view value) {
    const auto previous = SelectObject(dc, font);
    text(dc, x, y, value);
    SelectObject(dc, previous);
}

void drawHand(HDC dc, const Hand& hand, bool active, bool left) {
    const auto color = active ? accent : RGB(144, 160, 172);
    const auto pen = CreatePen(PS_SOLID, 5, color);
    const auto fill = CreateSolidBrush(color);
    const auto oldPen = SelectObject(dc, pen);
    const auto oldFill = SelectObject(dc, fill);
    constexpr std::array starts{POINT{-53, 12}, POINT{-28, -10}, POINT{0, -16}, POINT{26, -10}, POINT{50, 4}};
    constexpr std::array length{26.0f, 30.0f, 35.0f, 32.0f, 25.0f};
    constexpr int center = 230, base = 398;
    const float direction = left ? -1.0f : 1.0f;
    for (std::size_t finger = 0; finger < starts.size(); ++finger) {
        auto x = static_cast<float>(starts[finger].x);
        auto y = static_cast<float>(starts[finger].y);
        const float bend = active ? hand.curl[finger] : 0;
        const float splay = (static_cast<float>(finger) - 2) * .19f;
        MoveToEx(dc, center, base + 45, nullptr);
        LineTo(dc, center + static_cast<int>(direction*x), base + static_cast<int>(y));
        for (unsigned joint = 0; joint < 3; ++joint) {
            const auto flex = bend * static_cast<float>(joint + 1);
            const auto segment = length[finger] * (1.0f - joint * .17f);
            x += segment * (std::sin(splay) + flex * .15f);
            y -= segment * std::cos(flex * .82f);
            const auto px = center + static_cast<int>(direction*x);
            const auto py = base + static_cast<int>(y);
            LineTo(dc, px, py);
            Ellipse(dc, px-3, py-3, px+4, py+4);
        }
    }
    SelectObject(dc, oldFill);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);
}

void within(HDC dc, RECT bounds, std::wstring_view value, COLORREF color = secondary) {
    SetTextColor(dc, color);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &bounds, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
}

void paintDesktopTab(const DRAWITEMSTRUCT& item, bool selected) {
    const auto dc = item.hDC;
    const bool pressed = item.itemState & ODS_SELECTED;
    const bool focused = item.itemState & ODS_FOCUS;
    const auto area = item.rcItem;
    const auto fill = CreateSolidBrush(desktopBackground);
    FillRect(dc, &area, fill);
    DeleteObject(fill);
    card(dc, area, selected ? RGB(16, 76, 79) : pressed ? RGB(222, 236, 236) : RGB(239, 244, 247));
    wchar_t label[48]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? RGB(255, 255, 255) : ink);
    auto labelArea = area;
    DrawTextW(dc, label, -1, &labelArea, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (focused) {
        InflateRect(&labelArea, -5, -5);
        DrawFocusRect(dc, &labelArea);
    }
    SelectObject(dc, previousFont);
}

void paintDesktop(HWND window, HBRUSH background, HFONT font, HFONT title, HFONT section,
                  const Snapshot& snapshot, const TrackingSnapshot& tracking, std::size_t side,
                  DesktopPage page, const std::wstring& status) {
    PAINTSTRUCT paint{};
    const auto target = BeginPaint(window, &paint);
    const auto dc = CreateCompatibleDC(target);
    const auto bitmap = CreateCompatibleBitmap(target, desktopWidth, desktopHeight);
    const auto oldImage = SelectObject(dc, bitmap);
    RECT rect{0, 0, desktopWidth, desktopHeight};
    FillRect(dc, &rect, background);
    const auto oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    heading(dc, title, 30, 17, L"OpenCap");
    text(dc, 32, 57, snapshot.demo ? L"Simulated gloves" : L"Glove control for SteamVR", secondary);

    const auto time = now();
    for (std::size_t i = 0; i < handCount; ++i) {
        const auto& hand = snapshot.hands[i];
        const auto x = 30 + static_cast<int>(i)*430;
        const auto live = isLive(snapshot, static_cast<Side>(i), time);
        card(dc, {x, 86, x+410, 160}, i == side ? RGB(224, 242, 239) : RGB(240, 245, 248));
        const auto connection = live ? L"Live" : hand.receiver ? L"Glove offline" : L"No receiver";
        const auto battery = live && hand.battery >= 0 ? std::format(L"   Battery {:.0f}%", hand.battery*100) : L"";
        text(dc, x+20, 98, std::wstring(connection) + battery, live ? accent : secondary);
        const auto link = hand.channel < 0 ? radioSignalLabel(hand, live) :
            std::format(L"Ch {}  ·  {}", hand.channel, radioSignalLabel(hand, live));
        within(dc, {x+151, 131, x+418, 151}, link);
    }

    const auto& hand = snapshot.hands[side];
    const auto active = isLive(snapshot, static_cast<Side>(side), time);
    const auto prompt = calibrationPrompt(snapshot.calibrationGuide, time);
    card(dc, {30, 231, 430, 544});
    card(dc, {450, 231, 890, 544});

    switch (page) {
    case DesktopPage::gloves: {
        heading(dc, section, 50, 249, L"Live hand");
        within(dc, {50, 279, 409, 300}, active
            ? std::format(L"Controller {}  ·  {:.0f} fps", hand.controllerRevision ? std::to_wstring(hand.controllerRevision)+L".0" : L"—", hand.fps)
            : L"Waiting for glove data");
        drawHand(dc, hand, active, side == 0);
        if (active) {
            const auto input = std::format(L"Trigger {:.0f}%  ·  Grip {:.0f}%  ·  Trackpad {:.0f}%",
                                           100*hand.trigger, 100*hand.grip, 100*hand.pad);
            within(dc, {50, 452, 410, 474}, input);
        }
        heading(dc, section, 472, 249, L"Calibration");
        const auto bothLive = isLive(snapshot, Side::left, time) && isLive(snapshot, Side::right, time);
        const auto bothCalibrated = snapshot.hands[0].calibrated && snapshot.hands[1].calibrated;
        const auto calibrationStatus = !bothLive ? L"Connect both gloves to calibrate"
            : !handCalibrationReady(snapshot, time) ? L"Finish joystick calibration first"
            : bothCalibrated ? L"Both gloves calibrated" : L"Ready to calibrate both gloves";
        within(dc, {472, 276, 868, 297}, prompt.empty() ? calibrationStatus : prompt, accent);
        text(dc, 472, 347, hand.calibrationStep == 4 ? L"Rotate the stick through its full range." : L"Joystick calibration", secondary);
        heading(dc, section, 472, 421, L"Hand and controller response");
        if (active && hand.quality.measured)
            within(dc, {680, 503, 870, 525}, std::format(L"{} channels graded Poor", hand.quality.lowChannels));
        break;
    }
    case DesktopPage::devices: {
        heading(dc, section, 50, 249, L"Pairing and radio");
        text(dc, 50, 286, hand.pairing ? L"Pairing is active on this glove." : L"Select a glove above to pair it.", secondary);
        const auto channel = hand.channel >= 0 ? std::to_wstring(hand.channel) : L"—";
        within(dc, {50, 382, 410, 406}, std::format(L"Channel {}  ·  {}", channel, radioSignalLabel(hand, active)));
        heading(dc, section, 472, 249, L"Vibration");
        text(dc, 472, 285, active && hand.controllerRevision == 2
            ? L"Test the Controller 2.0 actuator." : L"Test the selected glove’s vibration.", secondary);
        heading(dc, section, 472, 375, L"Controller options");
        if (hand.firmware[0]) within(dc, {50, 488, 409, 512}, L"Firmware " + widen(hand.firmware.data()));
        break;
    }
    case DesktopPage::tracking: {
        heading(dc, section, 50, 249, L"Pose source");
        std::wstring source = L"Waiting for SteamVR";
        if (trackingCurrent(tracking, time)) {
            const auto tracker = tracking.selected[side];
            source = tracker == noTracker ? L"No active tracker" : std::format(L"Active device  #{}", tracker);
        }
        within(dc, {50, 285, 409, 309}, source, accent);
        text(dc, 50, 441, L"Tracker preset", secondary);
        heading(dc, section, 472, 249, L"Tracker alignment");
        text(dc, 472, 285, L"Grip position in metres", secondary);
        text(dc, 472, 363, L"Rotation in degrees", secondary);
        for (int axis = 0; axis < 3; ++axis) {
            const std::wstring name(1, L"XYZ"[axis]);
            text(dc, 472+135*axis, 304, name, secondary);
            text(dc, 472+135*axis, 381, name, secondary);
        }
        break;
    }
    case DesktopPage::streaming:
        heading(dc, section, 50, 249, L"VMC");
        heading(dc, section, 472, 249, L"OSC");
        text(dc, 50, 286, L"Hand motion and controller data", secondary);
        text(dc, 472, 286, L"VRChat and custom destinations", secondary);
        text(dc, 50, 324, L"Destination IPv4", secondary);
        text(dc, 305, 324, L"Port", secondary);
        text(dc, 472, 324, L"Destination IPv4", secondary);
        text(dc, 762, 324, L"Port", secondary);
        text(dc, 50, 404, L"Set a port to 0 to turn off that output.", secondary);
        break;
    case DesktopPage::settings:
        heading(dc, section, 50, 249, L"Startup and shortcuts");
        text(dc, 50, 285, L"F1 starts calibration for connected gloves.", secondary);
        heading(dc, section, 472, 249, L"Firmware");
        text(dc, 472, 285, L"Inspect an image or update a connected device.", secondary);
        if (!snapshot.demo) heading(dc, section, 472, 385, L"Application");
        break;
    }

    const auto actionText = hand.calibrationStep == 4 ? L"Move the joystick around its full range, then select Finish."
        : hand.pairing ? L"Pairing is active. Put the glove into pairing mode." : L"";
    const auto message = page != DesktopPage::gloves && !prompt.empty() ? prompt : !status.empty() ? status : hand.message[0] ? widen(hand.message.data())
        : *actionText ? actionText : !snapshot.demo && !snapshot.receiverSupported ? L"Physical receiver support is disabled in this build." : L"";
    if (!message.empty()) {
        card(dc, {30, 552, 890, 591}, RGB(230, 244, 242));
        within(dc, {48, 563, 874, 587}, message, accent);
    }

    BitBlt(target, 0, 0, desktopWidth, desktopHeight, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldImage);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

static std::filesystem::path csvDestination(HWND window, const wchar_t* filename) {
    wchar_t destination[MAX_PATH]{};
    lstrcpynW(destination, filename, MAX_PATH);
    OPENFILENAMEW request{};
    request.lStructSize = sizeof(request);
    request.hwndOwner = window;
    request.lpstrFile = destination;
    request.nMaxFile = MAX_PATH;
    request.lpstrFilter = L"CSV file\0*.csv\0\0";
    request.lpstrDefExt = L"csv";
    request.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&request) ? std::filesystem::path{destination} : std::filesystem::path{};
}

std::optional<bool> exportHandSample(HWND window, Side side, const Hand& hand) {
    const auto path = csvDestination(window, L"opencap-hand.csv");
    if (path.empty()) return std::nullopt;
    std::ofstream output{path};
    output.imbue(std::locale::classic());
    output << "hand,finger,joint,curl,qx,qy,qz,qw\n";
    for (std::size_t i = 0; i < hand.joints.size(); ++i) {
        const auto q = hand.joints[i].rotation;
        output << (side == Side::right ? "right" : "left") << ',' << i/3 << ',' << i%3 << ','
               << hand.curl[i/3] << ',' << q.x << ',' << q.y << ',' << q.z << ',' << q.w << '\n';
    }
    output.close();
    return static_cast<bool>(output);
}

std::optional<bool> exportCalibrationReport(HWND window, Side side, const CalibrationSamples& samples) {
    const auto path = csvDestination(window,
        side == Side::left ? L"opencap-left-calibration.csv" : L"opencap-right-calibration.csv");
    if (path.empty()) return std::nullopt;
    std::ofstream output{path};
    output.imbue(std::locale::classic());
    writeCalibrationReport(output, side, samples);
    output.close();
    return static_cast<bool>(output);
}

void showFirmwareImage(HWND owner) {
    wchar_t selected[1024]{};
    OPENFILENAMEW request{};
    request.lStructSize = sizeof(request);
    request.hwndOwner = owner;
    request.lpstrFile = selected;
    request.nMaxFile = static_cast<DWORD>(std::size(selected));
    request.lpstrFilter = L"Firmware images\0*.bin\0All files\0*.*\0\0";
    request.lpstrTitle = L"Inspect a UDCAP firmware image";
    request.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&request)) return;
    try {
        const std::filesystem::path path(selected);
        auto image = readFirmware(path);
        constexpr std::array targets{L"Unknown", L"Receiver", L"Left glove", L"Right glove"};
        const auto target = targets[static_cast<std::size_t>(firmwareTarget(path))];
        const auto details = [&] {
            const auto scheme = image.signature == ImageSignature::rsa2048 ? L"RSA-2048 PSS" : L"ECDSA P-256";
            return std::format(L"{}\n\nTarget indicated by filename  {}\nImage version  {}.{}.{}+{}\n"
                L"Payload  {} bytes\nSHA-256  matches\nSignature  {} ({})", path.filename().wstring(), target,
                image.major, image.minor, image.revision, image.build, image.payloadBytes, scheme,
                image.signatureVerified ? L"verified" : L"unverified");
        };
        const auto prompt = details() + L"\n\nVerify the signature with a trusted public key?";
        if (MessageBoxW(owner, prompt.c_str(), L"OpenCap firmware image",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION) != IDYES) return;

        selected[0] = L'\0';
        request.lpstrFilter = L"Public keys\0*.pem;*.der;*.pub\0All files\0*.*\0\0";
        request.lpstrTitle = L"Choose a trusted firmware signing public key";
        request.nFilterIndex = 1;
        if (!GetOpenFileNameW(&request)) return;
        image = readFirmware(path, selected);
        MessageBoxW(owner, details().c_str(), L"OpenCap firmware image", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& error) {
        MessageBoxW(owner, widen(error.what()).c_str(), L"OpenCap firmware image", MB_OK | MB_ICONERROR);
    }
}
}
