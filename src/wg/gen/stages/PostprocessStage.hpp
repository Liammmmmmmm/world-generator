#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 6, after the hydrology. Does nothing yet.
// TODO: coastal cliffs.
class PostprocessStage final : public Stage {
public:
    explicit PostprocessStage(StageParams&) {}
    void run(WorldState&, const StageContext&) override {}
};

} // namespace wg
