// The Keyer node's measurement and its range curve, shared with the panel's range graph.
#pragma once

#include <array>
#include <string>

#include "core/Image.h"

namespace strata { class Node; }
namespace strata::keyer {

std::string channelSetting(const Node& node, const std::string& knob, const std::string& fallback);
std::array<int, 4> layerChannels(const Image& image, const std::string& layer);
// What the operation measures in a pixel (r, g, b, a): luminance, a channel, saturation, min / max,
// or screen dominance (the screen channel minus the larger other channel).
float measure(const std::string& op, const float p[4], bool legacy = false);
// Zero below A, rising to 1 at B, holding through C, then falling to zero past D.
float curve(float v, float A, float B, float C, float D, bool smooth = false);
// At the upper endpoint, coincident C and D leave highlights opaque.
float matte(float v, float A, float B, float C, float D, bool legacy = false);

}  // namespace strata::keyer
