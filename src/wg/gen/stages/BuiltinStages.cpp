// Registration of every built-in stage type, and the default island pipeline.
// To add a stage: write a Stage subclass in gen/stages/, register it below, and
// append it to the default pipeline (or use its type in a config "pipeline").

#include "wg/gen/StageRegistry.hpp"
#include "wg/gen/stages/ErosionStage.hpp"
#include "wg/gen/stages/HydrologyStage.hpp"
#include "wg/gen/stages/IslandInitStage.hpp"
#include "wg/gen/stages/ParametersStage.hpp"
#include "wg/gen/stages/UpscaleStage.hpp"
#include "wg/gen/stages/VoxelizeStage.hpp"

namespace wg {

const StageRegistry& StageRegistry::builtin() {
    static const StageRegistry registry = [] {
        StageRegistry r;
        r.add<ParametersStage>("parameters");
        r.add<IslandInitStage>("island_init");
        r.add<ErosionStage>("erosion");
        r.add<UpscaleStage>("upscale");
        r.add<HydrologyStage>("hydrology");
        r.add<VoxelizeStage>("voxelize");
        return r;
    }();
    return registry;
}

const std::vector<std::string>& StageRegistry::defaultPipeline() {
    static const std::vector<std::string> stages{
        "parameters",
        "island_init",
        "erosion",
        "upscale",
        "hydrology",
        "voxelize",
    };
    return stages;
}

} // namespace wg
