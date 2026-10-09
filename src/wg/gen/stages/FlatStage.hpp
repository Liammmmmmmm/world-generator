#pragma once

#include <optional>
#include <string>

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Sets a layer to a constant value.
//   "layer": target layer (default "height")
//   "value": constant (default seaLevel + 2)
class FlatStage final : public Stage {
public:
    explicit FlatStage(StageParams& params);
    void run(WorldState& world, const StageContext& context) override;

private:
    std::string layer_;
    std::optional<float> value_;
};

} // namespace wg
