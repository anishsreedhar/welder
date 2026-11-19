// Detail mask: where an image has texture (foliage, bark, fabric, hair) as opposed to flat areas
// (sky, walls, soft gradients). 0 = flat, toward 1 = busy. Built from two measures of the
// display-encoded luminance (retime::luminance, so shadows and highlights count the way they look):
//   edges     the Sobel gradient magnitude (outlines and fine detail)
//   contrast  the local standard deviation over a window (texture that is busy but soft)
// combined as 1 - exp(-gain * (edges * edge_weight + contrast * contrast_weight)).
//
// Sizes are in full-resolution pixels and the gradient is measured per full-resolution pixel, so a
// proxy render gives (about) the same mask.
#pragma once

#include <vector>

#include "../core/Image.h"

namespace strata::fx {

struct DetailOptions {
    double edgeWeight = 1;      // weight of the Sobel gradient
    double contrastWeight = 1;  // weight of the local standard deviation
    double radius = 4;          // contrast window radius, full-res pixels
    double gain = 12;           // how quickly the mask reaches 1
    double scale = 1;           // render scale (Engine::scale)
};

// How far around a region the source must reach (render pixels) for detailMask to see everything.
int detailPad(const DetailOptions& o);

// The mask for `region` (row-major, bottom row first, region.width() * region.height() values),
// from `src`, which should cover region padded by detailPad (reads clamp to src's bounds).
std::vector<float> detailMask(const Image& src, const Box& region, const DetailOptions& o);

}  // namespace strata::fx
