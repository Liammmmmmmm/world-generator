#pragma once

#include <optional>
#include <string>

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Pulls a layer toward `floor` near the map borders, turning the terrain into
// a continent surrounded by ocean. Distance is normalized: 0 at the center,
// 1 at the middle of each edge.
//   "layer": (default "height")
//   "start": distance where the falloff begins (default 0.5)
//   "end":   distance where the value reaches `floor` (default 0.95)
//   "floor": ocean floor value (default seaLevel - 24)
class IslandMaskStage final : public Stage {
public:
    explicit IslandMaskStage(StageParams& params);
    void run(WorldState& world, const StageContext& context) override;

private:
    std::string layer_;
    float start_;
    float end_;
    std::optional<float> floor_;
};

} // namespace wg
