#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 3: particle-based hydraulic erosion of the island (erosion.particles,
// see wg/erosion/). Reads "h" and the flags of the initialization, erodes "h"
// step by step, and produces "sediment", "discharge_norm", "eroded",
// "momentum" and "water" (the lakes kept in deep depressions, erosion.lakes).
// Follows StageContext::step, so the viewer can pause it or watch
// it step by step.
class ErosionStage final : public Stage {
public:
    explicit ErosionStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
