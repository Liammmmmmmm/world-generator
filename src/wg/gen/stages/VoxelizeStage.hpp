#pragma once

#include <string>

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Builds the 3D block volume from a heightmap: bedrock, stone, a filler layer
// (dirt/sand/gravel), a surface block and water up to sea level.
//   "heightLayer": (default "height")
//   "dirtDepth":   filler thickness under the surface (default 3)
//   "beachHeight": blocks above sea level still covered in sand (default 2)
class VoxelizeStage final : public Stage {
public:
    explicit VoxelizeStage(StageParams& params);
    void run(WorldState& world, const StageContext& context) override;

private:
    std::string heightLayer_;
    int dirtDepth_;
    int beachHeight_;
};

} // namespace wg
