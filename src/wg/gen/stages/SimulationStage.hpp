#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 3: evolves the island state of step 2 over simulation.total_duration_years,
// in steps of simulation.dt_years, on the coarse grid. Each step:
//   1. time parameters (sea level, uplift rates of the phases)
//   2. tectonic uplift
//   3. climate            (part 4)
//   4. glaciers           (part 5)
//   5. water routing + fluvial erosion (stream power law, implicit)
//   6. hillslopes         (part 2)
//   7. marine erosion     (part 4)
//   8. checks, logs, live preview
// Every process can be switched off (simulation.processes.*).
class SimulationStage final : public Stage {
public:
    explicit SimulationStage(StageParams&) {}
    void run(WorldState& world, const StageContext& context) override;
};

} // namespace wg
