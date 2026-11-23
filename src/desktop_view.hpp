#pragma once

#include "model.hpp"
#include "desktop_layout.hpp"

#include <windows.h>
#include <optional>
#include <string>
#include <string_view>

namespace opencap {
inline constexpr COLORREF desktopBackground = RGB(248, 249, 250);
void paintDesktop(HWND window, const DesktopLayout& layout, int scroll, unsigned dpi, HFONT font, HFONT section,
                  const Snapshot& snapshot, const TrackingSnapshot& tracking, std::size_t side,
                  DesktopPage page, const std::wstring& status);
enum class DesktopButton { standard, navigation, selectedPage, selectedHand, adjustment, viewport };
void paintDesktopButton(const DRAWITEMSTRUCT& item, DesktopButton style);
std::optional<bool> exportHandSample(HWND window, Side side, const Hand& hand);
std::optional<bool> exportCalibrationReport(HWND window, Side side, const CalibrationSamples& samples);
void showFirmwareImage(HWND owner);
int runDesktop(HINSTANCE instance, int show, bool demo, const std::wstring& session, bool isolated, HANDLE steamOverlay);
}
