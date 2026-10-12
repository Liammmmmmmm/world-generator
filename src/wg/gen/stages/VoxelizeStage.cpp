#include "wg/gen/stages/VoxelizeStage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "wg/core/WorldState.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

void VoxelizeStage::run(WorldState& world, const StageContext& context) {
    const IslandParams& p = world.params().values;
    const auto& v = p.voxelization;
    if (!v.enabled)
        return;

    const WorldInfo& info = context.info;
    const int width = info.width, length = info.length;
    const double sea = info.seaLevelM();
    const Grid2D<float>& h = world.layer(layers::kHeight);
    const Grid2D<float>* discharge =
        world.hasLayer(layers::kDischargeNorm) ? &world.layer(layers::kDischargeNorm) : nullptr;

    // Surface altitude of every block column, in blocks (fractional).
    std::vector<float> top(static_cast<std::size_t>(width) * length);
    const double toBlocks = p.world.verticalExaggeration / info.blockSizeM;
    for (int z = 0; z < length; ++z) {
        if (z % 64 == 0)
            context.progress(0.3f * static_cast<float>(z) / length);
        const double ym = (z + 0.5) * info.blockSizeM - info.lengthM() / 2.0;
        for (int x = 0; x < width; ++x) {
            const double xm = (x + 0.5) * info.blockSizeM - info.widthM() / 2.0;
            const double hm = sampleCubic(h, xm, ym, info.widthM(), info.lengthM());
            top[static_cast<std::size_t>(z) * width + x] = static_cast<float>(info.seaLevel + (hm - sea) * toBlocks);
        }
    }
    const auto topAt = [&](int x, int z) {
        return top[static_cast<std::size_t>(std::clamp(z, 0, length - 1)) * width + std::clamp(x, 0, width - 1)];
    };
    // Y of the surface block of a column (the highest solid block).
    const auto surfaceY = [&](int x, int z) {
        return std::clamp(static_cast<int>(std::floor(topAt(x, z))), info.minY + 1, info.maxY - 1);
    };

    VoxelVolume& voxels = world.createVoxels(Block::Air);
    const float maxGrassSlope = static_cast<float>(std::tan(v.grassMaxSlopeDeg * std::numbers::pi / 180.0));
    const int dirt = v.dirtDepthBlocks;
    const int chunk = VoxelVolume::kSectionSize;
    long belowWorld = 0, aboveWorld = 0; // columns clipped to the world height

    for (int cz = 0; cz < length; cz += chunk) {
        context.progress(0.3f + 0.65f * static_cast<float>(cz) / length);
        for (int cx = 0; cx < width; cx += chunk) {
            const int x1 = std::min(cx + chunk, width), z1 = std::min(cz + chunk, length);
            // Below the lowest surface of the chunk, minus the dirt, all is
            // stone: filled as whole (uniform) sections, at no memory cost.
            int lowest = info.maxY;
            for (int z = cz; z < z1; ++z)
                for (int x = cx; x < x1; ++x)
                    lowest = std::min(lowest, surfaceY(x, z));
            voxels.fillBox(cx, info.minY, cz, x1, info.minY + 1, z1, Block::Bedrock);
            const int stoneTop = lowest - dirt; // exclusive
            voxels.fillBox(cx, info.minY + 1, cz, x1, stoneTop, z1, Block::Stone);

            for (int z = cz; z < z1; ++z) {
                for (int x = cx; x < x1; ++x) {
                    const float t = topAt(x, z);
                    belowWorld += t < info.minY + 1;
                    aboveWorld += t >= info.maxY;
                    const int y = surfaceY(x, z);
                    const float slope = std::hypot(topAt(x + 1, z) - topAt(x - 1, z), topAt(x, z + 1) - topAt(x, z - 1)) / 2.0f;
                    const bool underwater = y < info.seaLevel;
                    if (slope <= maxGrassSlope) { // grass on dirt (dirt under water)
                        voxels.fillColumn(x, z, std::max(stoneTop, info.minY + 1), y - dirt, Block::Stone);
                        voxels.fillColumn(x, z, std::max(y - dirt, info.minY + 1), y, Block::Dirt);
                        voxels.set(x, y, z, underwater ? Block::Dirt : Block::Grass);
                    } else { // bare rock
                        voxels.fillColumn(x, z, std::max(stoneTop, info.minY + 1), y + 1, Block::Stone);
                    }
                    if (discharge && !underwater) {
                        const double xm = (x + 0.5) * info.blockSizeM - info.widthM() / 2.0;
                        const double zm = (z + 0.5) * info.blockSizeM - info.lengthM() / 2.0;
                        if (sampleCentered(*discharge, xm, zm, info.widthM(), info.lengthM()) >= v.riverMinDischarge)
                            voxels.set(x, y, z, Block::River);
                    }
                    if (underwater)
                        voxels.fillColumn(x, z, y + 1, info.seaLevel, Block::Water);
                }
            }
        }
    }
    voxels.compact();
    // Deep sea floors below the world are expected; cut summits are not.
    if (belowWorld > 0)
        context.log(std::to_string(belowWorld) + " sea floor columns raised to the bottom of the world (y " +
                    std::to_string(info.minY + 1) + ")");
    if (aboveWorld > 0)
        context.log(std::to_string(aboveWorld) + " columns cut at the top of the world (y " + std::to_string(info.maxY) +
                    "): lower world.vertical_exaggeration");
    context.progress(1.0f);
}

} // namespace wg
