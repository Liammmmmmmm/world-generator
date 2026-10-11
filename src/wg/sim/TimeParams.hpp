#pragma once

#include <vector>

#include "wg/params/IslandParams.hpp"

namespace wg {

// Sea level offset (m, relative to the present sea level) at time t (years):
// linear interpolation of timeline.sea_level_curve, held at both ends; 0 if empty.
double seaLevelOffsetM(const std::vector<IslandParams::SeaLevelPoint>& curve, double t);

// Activity of a tectonic phase at time t, in [0, 1]: a trapezoid over
// [start_years, end_years], with linear ramps of 5 % of the phase duration.
double phaseActivity(const IslandParams::TectonicPhase& phase, double t);

} // namespace wg
