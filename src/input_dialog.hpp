#pragma once

#include "model.hpp"

#include <windows.h>
#include <optional>

namespace opencap {
struct ControllerEdit {
    ButtonMap mapping{};
    std::array<bool, 4> enabled{};
    ButtonRanges ranges{};
};
std::optional<ControllerEdit> editInputs(HWND owner, const HandSettings& settings, const Hand& hand, bool live);
struct JointEdit { int joint{}; Vec3 offset{}; };
std::optional<JointEdit> editJoint(HWND owner, const std::array<Vec3, jointCount>& offsets);
struct CurveEdit { std::size_t channel{}; HandCurve curve{}; };
std::optional<CurveEdit> editHandCurve(HWND owner, const HandCurves& curves);
}
