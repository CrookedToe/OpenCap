#pragma once

#include "model.hpp"

#include <windows.h>
#include <optional>

namespace opencap {
std::optional<ButtonMap> editInputs(HWND owner, ButtonMap mapping);
struct JointEdit { int joint{}; Vec3 offset{}; };
std::optional<JointEdit> editJoint(HWND owner, const std::array<Vec3, jointCount>& offsets);
}
