#pragma once

#include "model.hpp"

#include <openvr_driver.h>

namespace opencap {
using VrHandSkeleton = std::array<vr::VRBoneTransform_t, 31>;
VrHandSkeleton makeVrHandSkeleton(Side side, const Hand& hand);
using HandPreview = std::array<std::array<Vec3, 5>, 5>;
HandPreview makeHandPreview(Side side, const Hand& hand);
}
