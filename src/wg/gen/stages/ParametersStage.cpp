#include "wg/gen/stages/ParametersStage.hpp"

#include "wg/core/Config.hpp"
#include "wg/core/WorldState.hpp"

namespace wg {

void ParametersStage::run(WorldState& world, const StageContext& context) {
    world.setParams(context.config.params);
}

} // namespace wg
