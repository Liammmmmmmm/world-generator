#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 2: builds the initial island state on the simulation grid, without
// any erosion: land mask, bathymetry, initial topography, boundary conditions
// and the surface material. The erosion starts from it.
//
// Produces the layers listed in wg/island/IslandLayers.hpp.
class IslandInitStage final : public Stage {
public:
    explicit IslandInitStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
