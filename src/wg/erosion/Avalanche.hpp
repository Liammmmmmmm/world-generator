#pragma once

#include "wg/erosion/ErosionMap.hpp"
#include "wg/erosion/Rock.hpp"

namespace wg::erosion {

// Thermal erosion (avalanche) around cell (i, j), just modified: with each of
// its 8 neighbors, the part of the height difference beyond the talus slope
// of the higher cell (talus · distance) is unstable, and `rate` times half of
// it slides down to the lower cell, where it becomes loose sediment. A cell
// that receives matter is checked in turn, over a bounded number of levels.
// FIXED cells take no part.
void avalanche(ErosionMap& map, const Rock& rock, float rate, int i, int j);

} // namespace wg::erosion
