#pragma once

#include <windows.h>
#include <array>
#include <string_view>
#include <vector>

namespace opencap {
enum class DesktopPage { gloves, devices, tracking, streaming, settings };
inline constexpr std::array desktopPages{L"Gloves", L"Devices", L"Tracking", L"Streaming", L"Settings"};
enum Control {
    leftHand = 100, rightHand, nextPose, cancelPose, centerStick, finishStick, pair,
    stopPair, pairBoth, vibrate, useHand, useJoystick, useHaptics,
    trackerNum, posX, posY, posZ, rotX, rotY, rotZ, applyPose, udpIp, vmcPort, oscPort,
    applyStream, saveCsv, dashboard, globalHotkey, handView, inputMapping, preferOpenCap, jointAdjustment,
    oscIp, importUdcap, quickJoystick, trackingPlan, useTrackingPlan, inspectImage, steamStartup, saveCalibration,
    checkUpdates, includeBetas, vibrateSecond, firmwareUpdates, handResponse,
    exportMenu, automaticCalibration, recordCalibration, openCalibrationLogs,
    glovesPage, devicesPage, trackingPage, streamingPage, settingsPage
};

enum class Caption { heading, label, connection, calibration, joystick, pairing, radio, firmware, controller, tracker, status };
struct DesktopCaption {
    RECT area;
    Caption kind;
    std::wstring_view text;
};
struct DesktopLayout {
    std::array<RECT, settingsPage + 1> controls{};
    std::vector<DesktopCaption> captions;
    std::vector<RECT> rules;
    RECT preview{}, spread{};
    int width{}, height{}, navHeight{};
};
DesktopLayout desktopLayout(int width, int height, DesktopPage page, bool demo, unsigned stage, bool guiding);
}
