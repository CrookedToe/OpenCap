#include "desktop_layout.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace opencap;

namespace {
void checkScaledText() {
    const auto dc = CreateCompatibleDC(nullptr);
    for (int dpi : {96, 144, 192}) {
        const auto font = CreateFontW(-MulDiv(15,dpi,96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                      0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        const auto previous = SelectObject(dc, font);
        for (int width : {390, 820}) {
            for (const auto& [id, label] : {std::pair{finishStick, L"Finish joystick range"},
                    {nextPose, L"Guided calibration"}, {streamingPage, L"Streaming"}, {dashboard, L"Open in SteamVR"}}) {
                const auto layout = desktopLayout(width, 640, DesktopPage::gloves, false, id == finishStick ? 4 : 0, false);
                const auto& r = layout.controls[id];
                SIZE extent{};
                GetTextExtentPoint32W(dc, label, lstrlenW(label), &extent);
                if (extent.cx + 6 > MulDiv(r.right-r.left, dpi, 96) || extent.cy > MulDiv(r.bottom-r.top, dpi, 96))
                    throw std::runtime_error("A scaled button caption does not fit");
            }
        }
        SelectObject(dc, previous);
        DeleteObject(font);
    }
    DeleteDC(dc);
}

void check(const DesktopLayout& layout) {
    std::vector<RECT> areas;
    for (int id = leftHand; id <= settingsPage; ++id) {
        auto box = layout.controls[id];
        if (IsRectEmpty(&box)) continue;
        // A combo's allocated height includes its popup list.
        if (id == trackerNum || id == trackingPlan) box.bottom = box.top + 30;
        areas.push_back(box);
    }
    for (const auto& caption : layout.captions) areas.push_back(caption.area);
    if (!IsRectEmpty(&layout.spread)) areas.push_back(layout.spread);
    for (std::size_t i = 0; i < areas.size(); ++i) {
        const auto& a = areas[i];
        if (IsRectEmpty(&a) || a.left < 0 || a.right > layout.width || a.top < 0 || a.bottom > layout.height)
            throw std::runtime_error("Control or label outside scrollable content");
        for (std::size_t j = i + 1; j < areas.size(); ++j) {
            RECT overlap{};
            if (IntersectRect(&overlap, &a, &areas[j])) {
                std::cerr << "Overlap at " << overlap.left << ',' << overlap.top << " in width " << layout.width << '\n';
                throw std::runtime_error("Overlapping controls or labels");
            }
        }
    }
}
}

int main() {
    try {
        checkScaledText();
        for (int width : {390, 460, 649, 650, 734, 735, 819, 820, 1040, 1920}) {
            for (int height : {270, 540, 690, 1050}) {
                for (int p = 0; p < static_cast<int>(desktopPages.size()); ++p) {
                    const auto page = static_cast<DesktopPage>(p);
                    for (unsigned stage : {0u, 2u, 4u}) {
                        auto layout = desktopLayout(width, height, page, false, stage, stage == 2);
                        check(layout);
                        if (page != DesktopPage::gloves) continue;
                        const auto& action = layout.controls[stage == 4 ? finishStick : stage == 2 ? cancelPose : nextPose];
                        if (width < 820 ? action.top < layout.spread.bottom : action.left <= layout.preview.right)
                            throw std::runtime_error("Glove controls did not reflow around preview");
                    }
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
