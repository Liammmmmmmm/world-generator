#pragma once

#include <cstdint>
#include <vector>

#include "wg/sim/Hillslope.hpp"

namespace wg {

// Marine diffusion of the sediment (after Yuan et al. 2019, EPSL):
//
//   dh/dt = div(D(z) grad h),   D(z) = D0 exp(-depth / depth_scale)
//
// over the sea cells, with two grain classes moved separately: the coarse
// one (gravel and sand, D0 = sand_d0, low: stays near the coast) and the fines
// (clay, D0 = fines_d0, high: carried offshore). Only sediment moves; the rock
// b never changes.
//
// For each class, one implicit step of the diffusion of h with that class's
// D gives the fluxes across the cell faces (the "trial"), as if the whole
// surface were made of it. Directions-split implicit scheme (implicit in x
// over each row, then implicit in y over each column, Thomas algorithm): with
// D0 up to ~20 m2/yr, dt ~ 5 ka and dx = 4 m, r = D dt / dx2 reaches 1e3-1e4,
// far beyond what Peaceman-Rachford damps without as many sub-steps, while
// this split is monotone at any r. The fluxes are then limited by what is
// available: cells are visited from the highest of the trial surface down,
// and each sends at most the sediment of that class it has, including what it
// just received (so sediment crosses bare rock in a single step), and only a
// share min(1, sediment / mobile_layer_m) of the face fluxes: in h-diffusion
// over a rock slope, a thin film would carry as much as a thick cover, so at
// an unbounded speed. The budget is exact, and nothing digs the rock.
//
// The coast has no flux (no sediment climbs onto the land). FIXED cells keep
// their height and absorb what reaches them (exported).
struct MarineSettings {
    double sandD0 = 0.0;
    double finesD0 = 0.0;
    double depthScaleM = 1.0;
    double mobileLayerM = 1.0; // the flux out of a cell is scaled by min(1, sediment / mobile layer)
    double seaLevel = 0.0;
    double dt = 0.0;
    double cellM = 0.0;
};

// `active`: the sea cells (not FIXED). Returns the exported volume (m3).
// `h` = b + sed is updated on the active cells.
double marineDiffusion(const Regolith& regolith, std::vector<double>& h, const std::vector<std::uint8_t>& active,
                       const std::vector<std::uint8_t>& fixed, int nx, int ny, const MarineSettings& settings);

} // namespace wg
