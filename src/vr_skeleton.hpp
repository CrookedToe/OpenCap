#pragma once

#include "model.hpp"

#include <openvr_driver.h>

namespace opencap {
using VrHandSkeleton = std::array<vr::VRBoneTransform_t, 31>;
VrHandSkeleton makeVrHandSkeleton(Side side, const Hand& hand);
}
