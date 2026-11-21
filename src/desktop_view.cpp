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
constexpr std::array<const wchar_t*, handCount> sideNames{L"Left glove", L"Right glove"};

void text(HDC dc, int x, int y, std::wstring_view value, COLORREF color = ink) {
    SetTextColor(dc, color);
    TextOutW(dc, x, y, value.data(), static_cast<int>(value.size()));
}

void drawHand(HDC dc, const Hand& hand, bool active) {
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

}

void paintDesktop(HWND window, HBRUSH background, HFONT font, HFONT title,
                  const Snapshot& snapshot, const TrackingSnapshot& tracking, std::size_t side, const std::wstring& status) {
    PAINTSTRUCT paint{};
    const auto target = BeginPaint(window, &paint);
    const auto dc = CreateCompatibleDC(target);
    const auto bitmap = CreateCompatibleBitmap(target, desktopWidth, desktopHeight);
    const auto oldImage = SelectObject(dc, bitmap);
    RECT rect{0, 0, desktopWidth, desktopHeight};
    FillRect(dc, &rect, background);
    const auto oldFont = SelectObject(dc, title);
    SetBkMode(dc, TRANSPARENT);
    text(dc, 30, 19, L"OpenCap");
    SelectObject(dc, font);
    text(dc, 30, 58, snapshot.demo ? L"Simulated gloves" : L"UDCAP glove control", accent);
    for (std::size_t i = 0; i < handCount; ++i) {
        const auto& hand = snapshot.hands[i];
        const auto x = 30 + static_cast<int>(i) * 425;
        const auto live = isLive(snapshot, static_cast<Side>(i), now());
        const auto link = live ? L"Streaming" : hand.receiver ? L"Receiver connected · glove offline" : L"No receiver";
        text(dc, x, 101, std::format(L"{}  /  {}", sideNames[i], link), live ? accent : ink);
        const auto battery = hand.battery >= 0 ? std::format(L"{:.0f}%", 100 * hand.battery) : L"—";
        const auto signal = hand.signal ? std::format(L"{} dBm", hand.signal) : L"—";
        const auto firmware = hand.firmware[0] ? widen(hand.firmware.data()) : L"—";
        text(dc, x, 124, std::format(L"Battery {}   ·   {:.0f} fps   ·   {}   ·   FW {}", battery, hand.fps, signal, firmware));
    }
    const auto& hand = snapshot.hands[side];
    const auto active = isLive(snapshot, static_cast<Side>(side), now());
    text(dc, 30, 211, L"LIVE HAND", accent);
    if (trackingCurrent(tracking, now())) {
        const auto tracker = tracking.selected[side];
        text(dc, 30, 237, tracker == noTracker ? L"No active SteamVR pose source"
            : std::format(L"SteamVR pose source  #{}", tracker));
    }
    drawHand(dc, hand, active);
    text(dc, 30, 476, std::format(L"Trigger {:.0f}%   Grip {:.0f}%   Trackpad {:.0f}%",
         100*hand.trigger, 100*hand.grip, 100*hand.pad));
    text(dc, 30, 498, std::format(L"Stick {:+.2f}, {:+.2f}   ·   Buttons 0x{:02X}", hand.stickX, hand.stickY, hand.buttons));
    if (active && hand.quality.measured)
        text(dc, 223, 568, std::format(L"{} of {} channels with low movement",
                    hand.quality.lowChannels, hand.quality.counts.size()), accent);
    text(dc, 422, 208, hand.calibrated ? L"CALIBRATION  /  READY" : L"CALIBRATION", accent);
    text(dc, 422, 363, hand.channel >= 0 ? std::format(L"Channel {}", hand.channel) : L"Channel —");
    text(dc, 422, 451, L"TRACKER ALIGNMENT", accent);
    text(dc, 422, 474, L"Tracker ID      Position X / Y / Z (m)       Rotation X / Y / Z (°)");
    const auto actionText = hand.calibrationStep == 4 ? L"Move the joystick around its full range, then select Finish."
        : hand.pairing ? L"Pairing is active. Put the glove into pairing mode." : L"";
    const auto message = !status.empty() ? status : hand.message[0] ? widen(hand.message.data())
        : *actionText ? actionText
        : !snapshot.demo && !snapshot.receiverSupported ? L"This build supports simulated input. Physical receiver support is disabled." : L"";
    text(dc, 30, 594, message, accent);
    text(dc, 30, 610, L"STREAM OUTPUT", accent);
    text(dc, 30, 632, L"VMC destination");
    text(dc, 215, 632, L"Port");
    text(dc, 314, 632, L"OSC destination");
    text(dc, 499, 632, L"Port");
    text(dc, 30, 695, L"Port 0 turns the stream off.");
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
        const auto image = readFirmware(path);
        constexpr std::array targets{L"Unknown", L"Receiver", L"Left glove", L"Right glove"};
        const auto target = targets[static_cast<std::size_t>(firmwareTarget(path))];
        const auto scheme = image.signature == ImageSignature::rsa2048 ? L"RSA-2048" : L"ECDSA P-256";
        const auto message = std::format(L"{}\n\nTarget indicated by filename  {}\nImage version  {}.{}.{}+{}\n"
            L"Payload  {} bytes\nSHA-256  matches\nSignature record  {}\n\nThis inspection checks the file structure and payload hash.",
            path.filename().wstring(), target, image.major, image.minor, image.revision, image.build,
            image.payloadBytes, scheme);
        MessageBoxW(owner, message.c_str(), L"OpenCap firmware image", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception& error) {
        MessageBoxW(owner, widen(error.what()).c_str(), L"OpenCap firmware image", MB_OK | MB_ICONERROR);
    }
}
}
