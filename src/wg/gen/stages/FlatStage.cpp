#include "wg/gen/stages/FlatStage.hpp"

#include "wg/core/WorldState.hpp"

namespace wg {

FlatStage::FlatStage(StageParams& params)
    : layer_(params.get<std::string>("layer", "height")), value_(params.getOptional<float>("value")) {}

void FlatStage::run(WorldState& world, const StageContext& context) {
    world.createLayer(layer_, value_.value_or(static_cast<float>(context.info.seaLevel + 2)));
}

} // namespace wg
