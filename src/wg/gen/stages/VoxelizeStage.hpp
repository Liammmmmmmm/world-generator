#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 6: turns the heightmap into blocks. "h" is already at the block
// resolution (upscale stage): one column per cell, filled with simple
// materials:
//   - bedrock at the bottom, stone below the surface;
//   - gentle slopes: grass on a few blocks of dirt (dirt under water);
//   - steep slopes (voxelization.grass_max_slope_deg): bare stone;
//   - water: rivers and lakes up to their surface (hydrology), the sea up to
//     sea level.
// Heights are scaled by world.vertical_exaggeration and clamped to the world.
class VoxelizeStage final : public Stage {
public:
    explicit VoxelizeStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
