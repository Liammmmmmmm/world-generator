#pragma once

#include <algorithm>
#include <cmath>

#include "wg/params/IslandParams.hpp"

namespace wg {

// Geometry of the coarse simulation grid: simulation.coarse_resolution cells
// across the map width, square cells, origin at the map center.
struct IslandGrid {
    int nx = 0;
    int ny = 0;
    double cellM = 0.0;   // cell size in meters
    double radiusM = 0.0; // island radius R

    explicit IslandGrid(const IslandParams& params)
        : nx(params.simulation.coarseResolution),
          cellM(params.world.width * params.world.blockSizeM / params.simulation.coarseResolution),
          radiusM(params.islandRadiusM()) {
        ny = std::max(1, static_cast<int>(std::lround(params.world.length * params.world.blockSizeM / cellM)));
    }

    // Position of the center of cell (i, j), in meters from the map center.
    double x(int i) const { return (i + 0.5 - nx * 0.5) * cellM; }
    double y(int j) const { return (j + 0.5 - ny * 0.5) * cellM; }

    bool isBorder(int i, int j) const { return i == 0 || j == 0 || i == nx - 1 || j == ny - 1; }
};

} // namespace wg
