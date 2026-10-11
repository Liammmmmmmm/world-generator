#pragma once

#include <cstdint>
#include <vector>

#include "wg/params/IslandParams.hpp"
#include "wg/sim/Hillslope.hpp"

namespace wg {

// Glaciers: shallow ice approximation (SIA) on a coarse grid, relaxed to
// equilibrium at each climate update (ice evolves in centuries, erosion in
// millennia), then glacial erosion on the fine grid at every step.
//
//   dH/dt = M - div(q),  q = -D grad(s),  s = bed + H
//   D     = Gamma H^(n+2) |grad s|^(n-1) + u_s H / |grad s|,   n = 3
//   Gamma = 2 A / (n + 2) (rho g)^n,  u_s = C tau_b^2,  tau_b = rho g H |grad s|
//   M     = P * accumulation_rate (where T < T_snow) - ablation_gradient * max(0, T)
//
// The grid: SIA diffusivities reach 1e4-1e5 m2/yr, so the explicit limit
// dt < dx2 / (4 D) is ~1e-4 yr on cells of 4 m (millions of sub-steps per
// step): the ice runs on cells of at least erosion.glacial.ice_grid_cell_m,
// its thickness and sliding velocity interpolated back to the fine grid.
//
// The scheme (Jarosch, Schoof & Anslow 2013): fluxes through the cell faces
// with a MUSCL reconstruction of the surface (minmod limiter) on each side;
// the bed at a face is the higher of the two cells, and the ice thickness at
// the face is the reconstructed surface above it, taken upstream. Ice cannot
// cross a ridge it does not top, and the flux never takes more than there is:
// the mass is conserved (a naive explicit scheme does neither).
class GlacierModel {
public:
    GlacierModel(const IslandParams& params, int nx, int ny, double cellM);

    struct Climate {
        double baseTemperatureC, offsetC, lapseRateCPerKm;
        double referenceLevel; // altitude of base_temperature_c (the world's sea level)
        double seaLevel;       // current sea level (calving)
    };

    // Relaxes the ice towards equilibrium for the fine surface `h` (without
    // ice) and precipitation `precipitation` (m/yr). Returns the sub-steps used.
    int relax(const std::vector<double>& h, const std::vector<double>& precipitation, const Climate& climate);
    // Melts all the ice (no cell below 0 °C): the melt water is spread over
    // `years` (added to the melt rate).
    void meltAll(double years);
    bool hasIce() const;

    // Fine-grid fields: ice thickness (m), sliding velocity (m/yr), melt
    // (m/yr of water, the ablation actually applied), mass balance (m/yr).
    void fineFields(std::vector<double>& ice, std::vector<double>& sliding, std::vector<double>& melt,
                    std::vector<double>& balance) const;

    // Moraines: `eroded` (m of column per fine cell, composition `debris`) is
    // carried down the ice surface on the coarse grid. Where the ice thins
    // below `threshold` (front, margins) it builds moraines, at most a few
    // meters per step; the rest goes on downhill as outwash, to the sea or off
    // the map (returned, m3). (Dropped all at once, the debris of a whole ice
    // cap made mounds kilometers high, which the ice flowed over and grew on.)
    double depositDebris(const Regolith& regolith, const std::vector<double>& eroded,
                         const std::vector<Composition>& debris, const std::vector<double>& fineIce, double threshold,
                         double seaLevel, std::vector<double>& h) const;

    // Mass budget since the start (m3): what came in and out, and the ice now.
    struct Budget {
        double accumulated = 0.0, ablated = 0.0, calved = 0.0, melted = 0.0;
        double clamped = 0.0; // ice added by resetting negative thicknesses to 0
    };
    const Budget& budget() const { return budget_; }
    double volume() const;

private:
    int fine(int ci, int cj, int& i0, int& j0, int& i1, int& j1) const;
    // Debris reaching the sea fills it up to 1 m below sea level, outwards from
    // `start` (fine cell); returns what does not fit (m3).
    double fillSea(const Regolith& regolith, int start, const Composition& mix, double seaLevel,
                   std::vector<double>& h) const;
    double bilinear(const std::vector<double>& field, int i, int j) const;

    const IslandParams::Erosion::Glacial& g_;
    int nx_, ny_, factor_, cnx_, cny_;
    double cellM_, coarseM_;
    double gamma_, rhoG_;
    std::vector<double> ice_, bed_, sliding_, melt_, balance_;
    Budget budget_;
};

} // namespace wg
