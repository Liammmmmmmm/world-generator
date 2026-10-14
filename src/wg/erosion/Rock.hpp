#pragma once

#include <map>
#include <string>
#include <vector>

#include "wg/erosion/ErosionMap.hpp"
#include "wg/island/Geology.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg::erosion {

// The ground as the erosion sees it, in the normalized frame: the rock under
// the loose sediment (geology.materials at the rock surface) and the sediment
// itself.
class Rock {
public:
    struct Frame {
        int nx = 0, ny = 0;
        double cellM = 1.0;        // cell size
        double heightScaleM = 1.0; // meters per normalized height unit
        double seaLevelM = 0.0;
    };

    // `geology` must outlive the Rock. The strata anchor of every cell is
    // computed once (lookups are then a search in the strata pile).
    Rock(const Geology& geology, const std::map<std::string, IslandParams::Material>& materials, const Frame& frame,
         double hardnessInfluence, double sedimentTalusAngleDeg, double underwaterTalusAngleDeg);

    // Material id at the rock surface of cell (i, j) (its height minus its sediment).
    int material(int i, int j, const ErosionCell& cell) const;

    // Rock erosion rate factor of a cell: erodibility^hardness_influence of
    // its material (the loose sediment erodes at the normal rate).
    float erodibility(int i, int j, const ErosionCell& cell) const { return erodibility_[material(i, j, cell)]; }

    // Steepest stable slopes (height per cell), from the talus angles: of the
    // rock at the cell's rock surface, and of loose sediment (lower under
    // water: the sea or a lake).
    float rockTalus(int i, int j, const ErosionCell& cell) const { return talus_[material(i, j, cell)]; }
    float sedimentTalus(bool underwater) const { return underwater ? underwaterTalus_ : sedimentTalus_; }

    // The lowest talus of all: no slope under it can be unstable.
    float minTalus() const { return minTalus_; }

private:
    const Geology& geology_;
    Frame frame_;
    std::vector<double> anchor_; // Geology::strataAnchor of each cell
    std::vector<float> erodibility_; // by material id
    std::vector<float> talus_;       // by material id
    float sedimentTalus_ = 0.0f;
    float underwaterTalus_ = 0.0f;
    float minTalus_ = 0.0f;
};

} // namespace wg::erosion
