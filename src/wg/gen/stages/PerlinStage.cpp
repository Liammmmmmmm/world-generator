#include "wg/gen/stages/PerlinStage.hpp"

#include "wg/core/WorldState.hpp"
#include "wg/noise/Perlin.hpp"

namespace wg {

PerlinStage::PerlinStage(StageParams& params)
    : layer_(params.get<std::string>("layer", "height")), wavelength_(params.get("wavelength", 256.0)),
      octaves_(params.get("octaves", 4)), persistence_(params.get("persistence", 0.5)),
      lacunarity_(params.get("lacunarity", 2.0)), amplitude_(params.get("amplitude", 32.0f)),
      offset_(params.get("offset", 0.0f)) {
    const auto op = params.choice("op", "set", {"set", "add", "multiply"});
    op_ = op == "set" ? Op::Set : op == "add" ? Op::Add : Op::Multiply;

    if (wavelength_ <= 0.0)
        throw ConfigError("parameter 'wavelength' must be positive");
    if (octaves_ < 1 || octaves_ > Perlin2D::kMaxOctaves)
        throw ConfigError("parameter 'octaves' must be in [1, " + std::to_string(Perlin2D::kMaxOctaves) + "]");
}

void PerlinStage::run(WorldState& world, const StageContext& context) {
    const Perlin2D perlin(context.seed);
    Grid2D<float>& grid = op_ == Op::Set ? world.createLayer(layer_) : world.editLayer(layer_);
    const double frequency = 1.0 / wavelength_;

    for (int z = 0; z < grid.length(); ++z) {
        for (int x = 0; x < grid.width(); ++x) {
            const auto n = perlin.fbm(x * frequency, z * frequency, octaves_, lacunarity_, persistence_);
            const float value = offset_ + amplitude_ * static_cast<float>(n);
            float& cell = grid.at(x, z);
            switch (op_) {
            case Op::Set: cell = value; break;
            case Op::Add: cell += value; break;
            case Op::Multiply: cell *= value; break;
            }
        }
    }
}

} // namespace wg
