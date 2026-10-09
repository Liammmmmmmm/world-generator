#pragma once

#include <string>

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Fractal Perlin noise combined into a 2D layer:
//   value = offset + amplitude * fbm(x / wavelength, z / wavelength)
//
//   "layer":       target layer (default "height")
//   "op":          "set" (creates the layer) | "add" | "multiply" (default "set")
//   "wavelength":  size of the largest features, in blocks (default 256)
//   "octaves":     number of noise layers summed (default 4)
//   "persistence": amplitude factor between octaves (default 0.5)
//   "lacunarity":  frequency factor between octaves (default 2)
//   "amplitude":   (default 32)
//   "offset":      (default 0)
class PerlinStage final : public Stage {
public:
    explicit PerlinStage(StageParams& params);
    void run(WorldState& world, const StageContext& context) override;

private:
    enum class Op { Set, Add, Multiply };

    std::string layer_;
    Op op_;
    double wavelength_;
    int octaves_;
    double persistence_;
    double lacunarity_;
    float amplitude_;
    float offset_;
};

} // namespace wg
