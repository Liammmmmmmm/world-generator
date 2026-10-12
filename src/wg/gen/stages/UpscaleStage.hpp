#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 4: brings "h" from the simulation grid to the block resolution (one
// cell per block column), by bicubic (Catmull-Rom) interpolation: smooth, and
// through the values of the grid. The following stages (hydrology,
// voxelization) work on the final terrain, block by block. The other layers
// stay on the simulation grid.
class UpscaleStage final : public Stage {
public:
    explicit UpscaleStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
