#include "wg/gen/stages/IslandMaskStage.hpp"

#include <algorithm>
#include <cmath>

#include "wg/core/WorldState.hpp"

namespace wg {

IslandMaskStage::IslandMaskStage(StageParams& params)
    : layer_(params.get<std::string>("layer", "height")), start_(params.get("start", 0.5f)),
      end_(params.get("end", 0.95f)), floor_(params.getOptional<float>("floor")) {
    if (start_ < 0.0f || end_ <= start_)
        throw ConfigError("parameters must satisfy 0 <= 'start' < 'end'");
}

void IslandMaskStage::run(WorldState& world, const StageContext& context) {
    Grid2D<float>& grid = world.editLayer(layer_);
    const float floor = floor_.value_or(static_cast<float>(context.info.seaLevel - 24));
    const float cx = (grid.width() - 1) * 0.5f, cz = (grid.length() - 1) * 0.5f;

    for (int z = 0; z < grid.length(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const float dx = (x - cx) / (grid.width() * 0.5f);
            const float dz = (z - cz) / (grid.length() * 0.5f);
            const float t = std::clamp((std::sqrt(dx * dx + dz * dz) - start_) / (end_ - start_), 0.0f, 1.0f);
            const float falloff = t * t * (3.0f - 2.0f * t); // smoothstep
            float& cell = grid.at(x, z);
            cell += (floor - cell) * falloff;
        }
    }
}

} // namespace wg
