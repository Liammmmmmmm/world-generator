#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 1: publishes the resolved generation parameters into the world state,
// where later stages read them (world.params()). The resolution itself happens
// when the config is loaded (see resolveIslandParams), so that invalid values
// are reported before anything runs.
class ParametersStage final : public Stage {
public:
    explicit ParametersStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
