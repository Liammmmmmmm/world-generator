#pragma once

#include <cstdint>
#include <vector>

#include "wg/sim/Hillslope.hpp"

namespace wg {

// Wave erosion of the coasts (after Sunamura 1992, Trenhaile 2000).
//
// Exposure of a coast cell (land with an ocean neighbour): rays over 360°
// cross the water until they hit land or reach the maximum fetch,
//   exposure = sum w(theta) min(distance, fetch_max) / fetch_max damping / sum w(theta),
//   w(theta) = max(0, cos(theta - swell direction))²,
//   damping  = exp(-width of the shallows crossed / platform_attenuation_m)
// (normalized to [0, 1]: 1 for a cape facing the open swell over deep water).
//
// Retreat by accumulator: each coast cell accumulates the volume the waves
// would remove, wave_erosion_coeff * energy * exposure / hardness * dx * dt
// (wave_erosion_coeff: m2 of cliff section per year and per meter of coast),
// and is destroyed down to the abrasion platform (sea level - platform depth)
// once it has accumulated its whole volume. The excess passes on to the land
// neighbours that become the new coast.
//
// Why an accumulator: lowering every coast cell a little at each step gives a
// gentle slope instead of a cliff. The accumulator makes the cliff retreat
// cell by cell while keeping its full height.
//
// The debris becomes sediment with the composition of what was destroyed. On
// a sheltered coast (energy * exposure up to sediment_export_threshold) it
// first builds beaches on the most sheltered sea cells nearby, up to 1 m above
// sea level. The rest, and all of it on an exposed coast, fills the sea up to
// 1 m below the abrasion platform outwards from the cliff, for the marine
// diffusion to carry on; what does not fit is carried off the map (exported):
// left on the platform, it would build land in front of the cliff and shield it.
struct CoastalSettings {
    double waveErosionCoeff = 0.0;
    double swellEnergy = 0.0;
    double swellDirectionDeg = 0.0;
    int fetchRayCount = 32;
    double fetchMaxM = 0.0;
    double platformDepthM = 0.0; // > 0: below sea level
    double platformAttenuationM = 0.0; // the waves are damped by exp(-shallows width / this)
    double exportThreshold = 0.0;
    double sedimentHardness = 0.1;
    double seaLevel = 0.0;
    double dt = 0.0;
    double cellM = 0.0;
};

class CoastalErosion {
public:
    CoastalErosion(int nx, int ny) : nx_(nx), ny_(ny), exposure_(static_cast<std::size_t>(nx) * ny, 0.0),
                                     accumulator_(static_cast<std::size_t>(nx) * ny, 0.0) {}

    // Recomputes the coast cells and their exposure. `sea`: cells of the
    // ocean (connected to the border).
    void updateExposure(const std::vector<double>& h, const std::vector<std::uint8_t>& sea,
                        const CoastalSettings& settings);

    // One step of cliff retreat. `hardness`: of the surface rock of each cell.
    // Returns the volume destroyed (m3). `h` is updated.
    double step(const Regolith& regolith, std::vector<double>& h, std::vector<std::uint8_t>& sea,
                const std::vector<std::uint8_t>& fixed, const std::vector<double>& hardness,
                const CoastalSettings& settings);

    const std::vector<double>& exposure() const { return exposure_; }
    // Debris of the last step carried off the map (m3).
    double exported() const { return exported_; }
    // Accumulated volume over the volume of the cell, in [0, 1], on the coast.
    void normalizedAccumulator(const std::vector<double>& h, const CoastalSettings& settings,
                               std::vector<double>& out) const;

private:
    bool isCoast(const std::vector<double>& h, const std::vector<std::uint8_t>& sea, int c, double seaLevel) const;
    double rayExposure(const std::vector<double>& h, int c, const CoastalSettings& s) const;
    double cellVolume(double h, const CoastalSettings& s) const;

    int nx_, ny_;
    std::vector<double> exposure_;    // of the coast cells, 0 elsewhere
    std::vector<double> accumulator_; // m3
    double exported_ = 0.0;
};

} // namespace wg
