#include "desktop_view.hpp"
#include "bridge.hpp"
#include "firmware_image.hpp"
#include "hand_drawing.hpp"

#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <locale>

namespace opencap {
namespace {
constexpr COLORREF ink = RGB(27, 35, 46);
constexpr COLORREF secondary = RGB(82, 94, 108);
constexpr COLORREF accent = RGB(12, 101, 123);
constexpr COLORREF rule = RGB(209, 215, 220);
constexpr COLORREF paper = RGB(255, 255, 255);
constexpr COLORREF viewport = RGB(28, 39, 49);
void fill(HDC dc, RECT bounds, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void label(HDC dc, RECT bounds, std::wstring_view value, COLORREF color = ink,
           UINT flags = DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER) {
    SetTextColor(dc, color);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &bounds, flags | DT_NOPREFIX);
}

void drawHand(HDC dc, RECT canvas, Side side, const Hand& hand, bool live) {
    fill(dc, canvas, viewport);
    drawGlovePose(dc, {canvas.left+20, canvas.top+42, canvas.right-20, canvas.bottom-14},
                 side, hand, live, eulerDegrees({-10.f, 0, 0}));
    label(dc, {canvas.left+17, canvas.top+12, canvas.right-112, canvas.top+40},
          L"Hand preview", RGB(218, 228, 234));
}

void drawSpread(HDC dc, RECT area, const Hand& hand, bool live) {
    constexpr std::array names{L"Index–middle", L"Middle–ring", L"Ring–pinky"};
    for (int i = 0; i < 3; ++i) {
        const int x = area.left + (area.right-area.left+16)*i/3;
        const int right = area.left + (area.right-area.left+16)*(i+1)/3-16;
        const auto span = hand.quality.counts[splaySensorChannels[i]];
        const bool available = live && hand.splay.measured && (!hand.quality.measured || span >= minimumSplaySpan);
        const float value = hand.splay.gaps[i];
        const auto reading = available ? std::format(L"{:.0f}%", 100*value)
            : live ? (span ? L"Weak" : L"Learning") : L"—";
        label(dc, {x,area.top,right,area.top+20}, names[i], secondary);
        label(dc, {x,area.top+21,right,area.top+44}, reading);
        fill(dc, {x,area.top+48,right,area.top+51}, rule);
        if (available) fill(dc, {x,area.top+48,x+std::lround((right-x)*value),area.top+51}, accent);
    }
}
}

void paintDesktopButton(const DRAWITEMSTRUCT& item, DesktopButton style) {
    const auto dc = item.hDC;
    const auto area = item.rcItem;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool tab = style == DesktopButton::navigation || style == DesktopButton::selectedPage;
    const bool selected = style == DesktopButton::selectedPage || style == DesktopButton::selectedHand;
    const bool row = style == DesktopButton::adjustment;
    const bool dark = style == DesktopButton::viewport || style == DesktopButton::selectedHand;
    const int inset = MulDiv(12, GetDpiForWindow(item.hwndItem), 96);
    fill(dc, area, pressed ? RGB(207, 220, 226) : dark ? RGB(48, 66, 79) : disabled || row ? desktopBackground : paper);
    if (style == DesktopButton::selectedPage)
        fill(dc, {area.left+inset,area.bottom-3,area.right-inset,area.bottom}, accent);
    else if (row)
        fill(dc, {area.left,area.bottom-1,area.right,area.bottom}, rule);
    else if (!tab && !dark) {
        SetDCBrushColor(dc, rule);
        FrameRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    wchar_t caption[80]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    const auto previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    auto bounds = area;
    InflateRect(&bounds, row ? -2 : -3, 0);
    const auto color = disabled ? RGB(119, 130, 139) : dark && !pressed ? paper : selected ? accent : ink;
    label(dc, bounds, caption, color, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (row ? DT_LEFT : DT_CENTER));
    if (row) label(dc, {area.right-2*inset,0,area.right-inset,area.bottom}, L"›", secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (item.itemState & ODS_FOCUS) {
        bounds = area;
        InflateRect(&bounds, -4, -4);
        DrawFocusRect(dc, &bounds);
    }
    SelectObject(dc, previous);
}

void paintDesktop(HWND window, const DesktopLayout& layout, int scroll, unsigned dpi, HFONT font, HFONT section,
                  const Snapshot& snapshot, const TrackingSnapshot& tracking, std::size_t side,
                  DesktopPage page, const std::wstring& status) {
    PAINTSTRUCT paint{};
    const auto target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const auto dc = CreateCompatibleDC(target);
    const auto bitmap = CreateCompatibleBitmap(target, std::max(1L,client.right), std::max(1L,client.bottom));
    const auto oldImage = SelectObject(dc, bitmap);
    fill(dc, client, desktopBackground);
    const auto saved = SaveDC(dc);
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 96, 96, nullptr);
    SetViewportExtEx(dc, dpi, dpi, nullptr);
    SetWindowOrgEx(dc, 0, scroll, nullptr);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font);
    fill(dc, {0,0,layout.width,layout.navHeight}, paper);
    fill(dc, {0,layout.navHeight,layout.width,layout.navHeight+1}, rule);
    const auto& hand = snapshot.hands[side];
    const auto time = now();
    const auto active = isLive(snapshot, static_cast<Side>(side), time);
    const auto prompt = calibrationPrompt(snapshot.calibrationGuide, time);
    if (page == DesktopPage::gloves) {
        drawHand(dc, layout.preview, static_cast<Side>(side), hand, active);
        drawSpread(dc, layout.spread, hand, active);
    }
    for (auto line : layout.rules) fill(dc, line, rule);
    for (const auto& entry : layout.captions) {
        std::wstring text(entry.text);
        auto color = secondary;
        switch (entry.kind) {
        case Caption::heading: color = ink; break;
        case Caption::label: break;
        case Caption::connection:
            text = active ? (snapshot.demo ? L"Simulated glove" : L"Connected") : hand.receiver ? L"Glove offline" : L"No receiver";
            if (active && hand.battery >= 0) text += std::format(L"    ·    {:.0f}% battery", 100*hand.battery);
            if (active && hand.fps > 0) text += std::format(L"    ·    {:.0f} fps", hand.fps);
            break;
        case Caption::calibration:
            text = !prompt.empty() ? prompt
                : !snapshot.automaticHandCalibration ? (active && hand.calibrated ? L"Using fixed calibration" : L"Automatic calibration off")
                : !active ? L"Automatic when connected"
                : !hand.calibrated ? L"Open and close your hand to learn its range." : L"Adjusting live";
            if (active && hand.calibrated && prompt.empty() && hand.quality.measured && hand.quality.lowChannels) {
                const auto pending = std::ranges::count(hand.quality.counts, 0);
                text += pending ? std::format(L"   ·   {} of 12 ranges learned", 12-pending)
                                : std::format(L"   ·   {} weak channels", hand.quality.lowChannels);
            }
            break;
        case Caption::joystick: text = L"Rotate the stick through its full range."; break;
        case Caption::pairing: if (hand.pairing) text = L"Waiting for glove pairing…"; break;
        case Caption::radio:
            if (hand.pendingChannel >= 0) text = std::format(L"Switching to channel {}", hand.pendingChannel);
            else if (hand.channel >= 0) {
                text = std::format(L"Channel {}", hand.channel);
                if (active && hand.signal < 0) text += L"   ·   " + radioSignalLabel(hand, true);
            } else text = hand.receiver ? L"Reading channel…" : L"No receiver";
            break;
        case Caption::firmware: if (hand.firmware[0]) text = L"Firmware  " + widen(hand.firmware.data()); break;
        case Caption::controller: if (active && hand.controllerRevision) text = std::format(L"Revision {}.0", hand.controllerRevision); break;
        case Caption::tracker:
            text = !trackingCurrent(tracking, time) ? L"Waiting for SteamVR"
                : tracking.selected[side] == noTracker ? L"No active tracker" : std::format(L"Active device #{}", tracking.selected[side]);
            break;
        case Caption::status:
            text = page != DesktopPage::gloves && !prompt.empty() ? prompt : !status.empty() ? status
                : hand.message[0] ? widen(hand.message.data())
                : !snapshot.demo && !snapshot.receiverSupported ? L"Physical receiver support is disabled in this build." : L"";
            break;
        }
        if (text.empty()) continue;
        SelectObject(dc, entry.kind == Caption::heading ? section : font);
        const bool wrap = entry.kind == Caption::status || entry.kind == Caption::calibration;
        label(dc, entry.area, text, color, wrap ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, saved);
    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right-paint.rcPaint.left,
           paint.rcPaint.bottom-paint.rcPaint.top, dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
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
