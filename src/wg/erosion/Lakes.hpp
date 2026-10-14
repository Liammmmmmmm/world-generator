#pragma once

#include "wg/erosion/ErosionMap.hpp"

namespace wg::erosion {

// Finds the lakes of the current terrain and sets the water level of the map:
// a depression (Priority-Flood from the sea and the map border) at least
// `minDepth` deep is a lake, filled up to its spill level; every other cell is
// at the sea level (0). Returns the number of lakes.
int updateLakes(ErosionMap& map, float minDepth);

} // namespace wg::erosion
