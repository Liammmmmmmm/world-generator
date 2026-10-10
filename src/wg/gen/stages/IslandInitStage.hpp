#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 2: builds the initial island state on the coarse grid, without any
// simulation (no erosion, no time loop): land mask, bathymetry, initial
// topography, uplift patterns of the tectonic phases, boundary conditions and
// the surface material. Step 3 (uplift + erosion) starts from it.
//
// Produces the layers listed in wg/island/IslandLayers.hpp.
class IslandInitStage final : public Stage {
public:
    explicit IslandInitStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
