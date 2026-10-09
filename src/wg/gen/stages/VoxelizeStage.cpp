#include "wg/gen/stages/VoxelizeStage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "wg/core/WorldState.hpp"

namespace wg {

VoxelizeStage::VoxelizeStage(StageParams& params)
    : heightLayer_(params.get<std::string>("heightLayer", "height")), dirtDepth_(params.get("dirtDepth", 3)),
      beachHeight_(params.get("beachHeight", 2)) {
    if (dirtDepth_ < 0)
        throw ConfigError("parameter 'dirtDepth' must be >= 0");
}

void VoxelizeStage::run(WorldState& world, const StageContext& context) {
    const WorldInfo& info = context.info;
    const Grid2D<float>& height = world.layer(heightLayer_);
    VoxelVolume& voxels = world.createVoxels(Block::Air);

    const auto topAt = [&](int x, int z) {
        return std::clamp(static_cast<int>(std::floor(height.at(x, z))), info.minY + 1, info.maxY - 1);
    };

    // Bulk-fill what every column shares; it ends up as cheap uniform sections.
    int lowestTop = std::numeric_limits<int>::max();
    for (int z = 0; z < info.length; ++z)
        for (int x = 0; x < info.width; ++x)
            lowestTop = std::min(lowestTop, topAt(x, z));
    const int sharedStoneTop = std::max(info.minY + 1, lowestTop - dirtDepth_);
    voxels.fillBox(0, info.minY, 0, info.width, info.minY + 1, info.length, Block::Bedrock);
    voxels.fillBox(0, info.minY + 1, 0, info.width, sharedStoneTop, info.length, Block::Stone);

    for (int z = 0; z < info.length; ++z) {
        for (int x = 0; x < info.width; ++x) {
            const int top = topAt(x, z);
            const Block surface = top < info.seaLevel - 6               ? Block::Gravel
                                  : top <= info.seaLevel + beachHeight_ ? Block::Sand
                                                                        : Block::Grass;
            const Block filler = surface == Block::Grass ? Block::Dirt : surface;
            const int fillerBottom = std::max(info.minY + 1, top - dirtDepth_);

            voxels.fillColumn(x, z, sharedStoneTop, fillerBottom, Block::Stone);
            voxels.fillColumn(x, z, fillerBottom, top, filler);
            voxels.set(x, top, z, surface);
            voxels.fillColumn(x, z, top + 1, info.seaLevel + 1, Block::Water);
        }
    }
    voxels.compact();
}

} // namespace wg
