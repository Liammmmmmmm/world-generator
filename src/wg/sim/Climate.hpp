#pragma once

#include <cstdint>
#include <vector>

#include "wg/params/IslandParams.hpp"

namespace wg {

// Temperature offset of the glaciations active at time t (°C, <= 0), each
// ramped in and out over 10 % of its span.
double glacialOffsetC(const std::vector<IslandParams::Glaciation>& glaciations, double t);

// Climate of the island on the simulation grid: temperature and orographic
// precipitation, by advection of moisture along the wind.
//
// Moisture q enters from the upwind edge at humidity * q_sat(T_sea) and is
// carried cell by cell downwind (cells sorted by dot(p, wind), upwind first;
// each takes the q of the point 1.5 cells upwind, by bilinear interpolation,
// so the four cells it reads are always done already). Each cell rains
//
//   P = q (base_rate dx + condensation_rate * rise) + max(0, q - q_sat(T))
//
// (rates per km of travel and per km of rise: independent of the grid), with
// rise = max(0, dot(wind, grad s)) * dx, then q -= P. Over the sea q relaxes
// back towards humidity * q_sat(T) (moisture_recharge_over_sea per km).
// q_sat = q0 exp(0.07 T): air cooled by the lapse rate on the relief holds less
// water, the excess falls. P is scaled to m/yr so that its mean over the land
// is base_precipitation_m_per_year (the relief redistributes the rain), then
// lightly blurred (the advection leaves streaks along the wind). wind.variability_deg: the mean of 3 passes,
// the wind turned by -v, 0 and +v.
class ClimateModel {
public:
    ClimateModel(const IslandParams& params, int nx, int ny, double cellM);

    // T (°C) at surface s = h + ice, for the given offset of the glaciations.
    void temperature(const std::vector<double>& surface, double offsetC, std::vector<double>& out) const;

    // Precipitation (m/yr). `sea`: cells of the sea (moisture recharge).
    void precipitation(const std::vector<double>& surface, const std::vector<double>& temperature,
                       const std::vector<std::uint8_t>& sea, double seaTemperatureC, std::vector<double>& out) const;

private:
    struct Pass {
        double wx, wy;          // direction the air moves to
        std::vector<int> order; // cells, upwind first
    };
    const IslandParams::Climate& climate_;
    int nx_, ny_;
    double cellM_;
    std::vector<Pass> passes_;
};

} // namespace wg
