#pragma once

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Step 4, after the erosion. Does nothing yet.
// TODO: lakes (depression filling), river extraction, coastal cliffs.
class PostprocessStage final : public Stage {
public:
    explicit PostprocessStage(StageParams&) {}
    void run(WorldState&, const StageContext&) override {}
};

} // namespace wg
