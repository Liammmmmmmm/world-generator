#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 5: turns the heightmap into blocks, at the world's block resolution.
// The heights of the simulation grid are upscaled by bicubic (Catmull-Rom)
// interpolation; each column is then filled with simple materials:
//   - bedrock at the bottom, stone below the surface;
//   - gentle slopes: grass on a few blocks of dirt (dirt under water);
//   - steep slopes (voxelization.grass_max_slope_deg): bare stone;
//   - a river marker block on river beds (discharge_norm above
//     voxelization.river_min_discharge, above sea level);
//   - water up to sea level.
// Heights are scaled by world.vertical_exaggeration and clamped to the world.
class VoxelizeStage final : public Stage {
public:
    explicit VoxelizeStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
