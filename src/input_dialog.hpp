#pragma once

#include "model.hpp"

#include <windows.h>
#include <optional>

namespace opencap {
std::optional<ButtonMap> editInputs(HWND owner, ButtonMap mapping);
}
