#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 5: rivers and lakes on the final terrain (hydrology.*), at the block
// resolution (after the upscale):
//   1. conditioning (Priority-Flood with hybrid breaching, Lindsay 2016):
//      from the outlets (sea, map border) inwards, lowest first. A depression
//      behind a small barrier (a sediment bar on the coast) is drained by a
//      channel cut through it, at most breach_max_depth_m deep and
//      breach_max_length_m long; deeper ones are filled: lakes. Every cell then
//      has a strictly lower neighbor, down to the sea;
//   2. flow to the steepest lower neighbor (D8, slightly jittered so that flat
//      or even slopes do not give straight lines), drained area accumulated
//      downstream (uniform rain for now);
//   3. a river starts where the drained area passes river_min_area_m2 (its
//      source, high up) and widens downstream (river_width_coeff * sqrt(area));
//      it is drawn as a band of that width, its bed carved under its surface,
//      its surface never above the lowest dry bank (no wall of water);
//   4. lakes: filled depressions at least lake_min_depth_m deep, covering
//      lake_min_area_m2, at the level of their lowest rim.
// Modifies "h" (cut channels, river beds) and produces water_surface and
// water (dry / sea / lake / river).
class HydrologyStage final : public Stage {
public:
    explicit HydrologyStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
