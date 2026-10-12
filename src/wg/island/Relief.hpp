#pragma once

#include <cstdint>
#include <functional>

#include "wg/core/Grid2D.hpp"
#include "wg/island/IslandGrid.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

// Mountains made by noise (relief.zones): each zone is an amplitude pattern in
// [0, 1] (a band along an axis for a range, an ellipse for a massif, a wide
// flat-topped patch for hills), domain-warped and modulated at low frequency,
// that multiplies a ridged multifractal noise:
//
//   relief = smoothstep(0, 1, mask) · Σ_zones amplitude(p) · height_m · (b + (1 − b) · ridged(p))
//
// with b = 0.35 (kRidgeBody): the ridges stand on a mountain mass.
//
// The ridged noise is stretched along the axis of a range, so that its crests
// follow the chain. Returns the added height in meters on the island grid.
// `progress` receives the fraction of rows done.
Grid2D<float> reliefHeights(const IslandParams& params, const IslandGrid& grid, const Grid2D<float>& mask,
                            std::uint64_t seed, const std::function<void(double)>& progress);

} // namespace wg
