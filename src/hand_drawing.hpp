#pragma once

#include "model.hpp"
#include <windows.h>

namespace opencap {
void drawGlovePose(HDC dc, RECT frame, Side side, const Hand& hand, bool live, Quaternion camera);
}
