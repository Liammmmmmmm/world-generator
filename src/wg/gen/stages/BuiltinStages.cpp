// Registration of every built-in stage type. To add a stage: write a Stage
// subclass in gen/stages/, add it below, and use its type in a config.

#include "wg/gen/StageRegistry.hpp"
#include "wg/gen/stages/FlatStage.hpp"
#include "wg/gen/stages/IslandMaskStage.hpp"
#include "wg/gen/stages/PerlinStage.hpp"
#include "wg/gen/stages/VoxelizeStage.hpp"

namespace wg {

const StageRegistry& StageRegistry::builtin() {
    static const StageRegistry registry = [] {
        StageRegistry r;
        // 2D (layers)
        r.add<FlatStage>("flat");
        r.add<PerlinStage>("perlin");
        r.add<IslandMaskStage>("island_mask");
        // 2D -> 3D
        r.add<VoxelizeStage>("voxelize");
        return r;
    }();
    return registry;
}

} // namespace wg
