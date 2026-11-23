#include "desktop_layout.hpp"

#include <algorithm>

namespace opencap {
namespace {
struct Column {
    int x, width;
    RECT box(int y, int h) const { return {x, y, x + width, y + h}; }
    Column cell(int index, int count, int gap = 8) const {
        const int space = width - gap * (count - 1);
        const int left = space * index / count;
        return {x + left + gap * index, space * (index + 1) / count - left};
    }
};
}

DesktopLayout desktopLayout(int width, int height, DesktopPage page, bool demo, unsigned stage, bool guiding) {
    DesktopLayout out;
    out.width = width;
    out.navHeight = 52;
    const bool stacked = width < 820;
    const bool narrow = width < 660;
    const int margin = narrow ? 16 : 28;
    const Column body{margin, width - 2 * margin};
    const auto control = [&](int id, Column col, int y, int h = 34) { out.controls[id] = col.box(y, h); };
    const auto caption = [&](Column col, int y, Caption kind, std::wstring_view text = {}, int h = 26) {
        out.captions.push_back({col.box(y, h), kind, text});
    };
    const auto heading = [&](Column col, int y, std::wstring_view text) { caption(col, y, Caption::heading, text); };
    const auto rule = [&](Column col, int y) { out.rules.push_back(col.box(y, 1)); };

    const Column tabs{narrow ? 0 : margin, narrow ? width : 465};
    for (int i = 0; i < static_cast<int>(desktopPages.size()); ++i)
        control(glovesPage + i, tabs.cell(i, 5, 0), 0, out.navHeight);
    control(dashboard, {width - margin - 131, 131}, narrow ? 67 : 10, 32);

    const bool handPage = page == DesktopPage::gloves || page == DesktopPage::devices || page == DesktopPage::tracking;
    int top = narrow ? 118 : 76;
    if (handPage) {
        const int y = narrow ? 66 : 72;
        control(leftHand, {margin, 88}, y);
        control(rightHand, {margin + 88, 88}, y);
        const bool below = width < 735;
        const int x = below ? margin : margin + 198;
        caption({x, width - x - margin}, below ? y + 39 : y + 3, Caption::connection);
        top = y + (below ? 83 : 57);
    }

    const int rightWidth = std::clamp(body.width / 3, 305, 440);
    const Column left{margin, stacked ? body.width : body.width - rightWidth - 32};
    const Column right = stacked ? body : Column{width - margin - rightWidth, rightWidth};
    int bottom = top;
    switch (page) {
    case DesktopPage::gloves: {
        const int previewHeight = stacked ? std::clamp(body.width * 2 / 3, 235, 365)
                                          : std::clamp(height - top - 186, 268, 680);
        out.preview = left.box(top, previewHeight);
        control(handView, {left.x + left.width - 101, 85}, top + 12, 28);
        const int end = top + previewHeight;
        heading({left.x, left.width - 100}, end + 17, L"Finger spread");
        out.spread = left.box(end + 50, 51);
        const int y = stacked ? end + 118 : top;
        heading(right, y, L"Calibration");
        caption(right, y + 33, stage == 4 ? Caption::joystick : Caption::calibration, {}, 42);
        if (stage == 4) {
            control(centerStick, right.cell(0, 2), y + 83);
            control(finishStick, right.cell(1, 2), y + 83);
        } else {
            control(guiding || (stage >= 1 && stage <= 3) ? cancelPose : nextPose, right.cell(0, 2), y + 83);
            control(centerStick, right.cell(1, 2), y + 83);
        }
        rule(right, y + 141);
        heading(right, y + 158, L"Adjustments");
        int row = y + 195;
        for (int id : {handResponse, jointAdjustment, inputMapping}) {
            control(id, right, row, 35);
            row += 35;
        }
        control(exportMenu, {left.x + left.width - 85, 85}, end + 14, 29);
        bottom = std::max(end + 102, row);
        break;
    }
    case DesktopPage::devices: {
        heading(left, top, L"Pairing");
        for (auto [id, i] : {std::pair{pair, 0}, {stopPair, 1}, {pairBoth, 2}})
            control(id, left.cell(i, 3), top + 40);
        caption(left, top + 79, Caption::pairing);
        heading(left, top + 125, L"Radio");
        caption(left, top + 165, Caption::radio);
        caption(left, top + 195, Caption::firmware);
        const int y = stacked ? top + 250 : top;
        heading(right, y, L"Vibration");
        control(vibrate, right.cell(0, 2), y + 40);
        control(vibrateSecond, right.cell(1, 2), y + 40);
        heading({right.x, 120}, y + 112, L"Controller");
        caption({right.x + 124, right.width - 124}, y + 112, Caption::controller);
        control(useJoystick, right, y + 149);
        control(useHaptics, right, y + 187);
        control(quickJoystick, right, y + 225);
        bottom = std::max(top + 223, y + 259);
        break;
    }
    case DesktopPage::tracking: {
        heading(left, top, L"Tracker");
        caption(left, top + 33, Caption::tracker);
        control(trackerNum, left, top + 69, 210);
        control(useHand, left, top + 108);
        control(preferOpenCap, left, top + 148);
        heading(left, top + 213, L"Tracker preset");
        control(trackingPlan, left, top + 253, 200);
        control(useTrackingPlan, left, top + 294);
        const int y = stacked ? top + 362 : top;
        heading(right, y, L"Tracker alignment");
        for (int group = 0; group < 2; ++group) {
            const int start = y + 40 + group * 100;
            caption(right, start, Caption::label, group ? L"Rotation (degrees)" : L"Position (metres)");
            for (int i = 0; i < 3; ++i) {
                const auto cell = right.cell(i, 3);
                caption(cell, start + 27, Caption::label, std::wstring_view(&L"XYZ"[i], 1), 22);
                control((group ? rotX : posX) + i, cell, start + 51, 30);
            }
        }
        control(applyPose, right, y + 248);
        bottom = std::max(top + 328, y + 282);
        break;
    }
    case DesktopPage::streaming: {
        for (int i = 0; i < 2; ++i) {
            const auto col = i ? right : left;
            const int y = top + (stacked && i ? 156 : 0);
            heading(col, y, i ? L"OSC" : L"VMC");
            const Column address{col.x, col.width - 86}, port{col.x + col.width - 76, 76};
            caption(address, y + 39, Caption::label, L"Destination IPv4");
            caption(port, y + 39, Caption::label, L"Port");
            control(i ? oscIp : udpIp, address, y + 69, 30);
            control(i ? oscPort : vmcPort, port, y + 69, 30);
            bottom = y + 116;
        }
        caption(body, bottom, Caption::label, L"Port 0 disables an output.");
        control(applyStream, {body.x, 158}, bottom + 37);
        bottom += 71;
        break;
    }
    case DesktopPage::settings: {
        heading(left, top, L"Startup and shortcuts");
        control(globalHotkey, left, top + 39);
        if (!demo) control(steamStartup, left, top + 79);
        const int calibration = top + (demo ? 100 : 140);
        heading(left, calibration, L"Calibration");
        control(automaticCalibration, left, calibration + 39);
        const int calibrationEnd = calibration + (demo ? 73 : 156);
        if (!demo) {
            control(recordCalibration, left, calibration + 77);
            control(openCalibrationLogs, {left.x, 158}, calibration + 119);
        }
        const int y = stacked ? calibrationEnd + 27 : top;
        heading(right, y, L"Firmware");
        control(inspectImage, right, y + 39);
        bottom = y + 73;
        if (!demo) {
            control(firmwareUpdates, right, y + 82);
            heading(right, y + 152, L"Application");
            control(checkUpdates, right, y + 191);
            control(includeBetas, right, y + 230);
            control(importUdcap, right, y + 280);
            bottom = y + 314;
        }
        bottom = std::max(bottom, calibrationEnd);
        break;
    }
    }
    caption(body, bottom + 18, Caption::status, {}, 54);
    out.height = bottom + 80;
    return out;
}
}
