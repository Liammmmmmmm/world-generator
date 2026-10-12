#include "wg/gen/stages/UpscaleStage.hpp"

#include "wg/core/WorldState.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

void UpscaleStage::run(WorldState& world, const StageContext& context) {
    const WorldInfo& info = context.info;
    const Grid2D<float> coarse = world.layer(layers::kHeight); // copy: the layer is replaced
    if (coarse.width() == info.width && coarse.length() == info.length)
        return; // already at the block resolution
    Grid2D<float>& h = world.createLayer(layers::kHeight, info.width, info.length, 0.0f, {LayerStyle::Height, {}});
    for (int z = 0; z < info.length; ++z) {
        if (z % 64 == 0)
            context.progress(static_cast<float>(z) / info.length);
        const double zm = (z + 0.5) * info.blockSizeM - info.lengthM() / 2.0;
        for (int x = 0; x < info.width; ++x) {
            const double xm = (x + 0.5) * info.blockSizeM - info.widthM() / 2.0;
            h.at(x, z) = sampleCubic(coarse, xm, zm, info.widthM(), info.lengthM());
        }
    }
    context.progress(1.0f);
}

} // namespace wg
