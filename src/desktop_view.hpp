#pragma once

#include "model.hpp"

#include <windows.h>
#include <optional>
#include <string>
#include <string_view>

namespace opencap {
inline constexpr int desktopWidth = 920, desktopHeight = 608;
inline constexpr COLORREF desktopBackground = RGB(248, 250, 252);
enum class DesktopPage { gloves, devices, tracking, streaming, settings };
void paintDesktop(HWND window, HBRUSH background, HFONT font, HFONT title, HFONT section,
                  const Snapshot& snapshot, const TrackingSnapshot& tracking, std::size_t side,
                  DesktopPage page, const std::wstring& status);
void paintDesktopTab(const DRAWITEMSTRUCT& item, bool selected);
std::optional<bool> exportHandSample(HWND window, Side side, const Hand& hand);
std::optional<bool> exportCalibrationReport(HWND window, Side side, const CalibrationSamples& samples);
void showFirmwareImage(HWND owner);
int runDesktop(HINSTANCE instance, int show, bool demo, const std::wstring& session, bool isolated, HANDLE steamOverlay);
}
