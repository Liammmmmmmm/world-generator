#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 4: rivers and lakes on the eroded terrain (hydrology.*), on the
// simulation grid:
//   1. depressions are filled (Priority-Flood, Barnes et al. 2014, with a
//      tiny slope so that water always has a way out): the water surface,
//      whose excess over the ground makes the lakes;
//   2. each cell drains to its steepest neighbor on that surface (D8), so
//      water goes through the lakes and always reaches the sea;
//   3. the drained area accumulates downstream (uniform rain for now);
//   4. a river starts where the drained area passes hydrology.river_min_area_m2
//      (its source, high in the mountains) and widens as its tributaries join.
// Produces water_surface, lake_depth, drainage, river_width, river_depth and
// the flow_direction byte layer, which the voxelization uses to draw them.
class HydrologyStage final : public Stage {
public:
    explicit HydrologyStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
