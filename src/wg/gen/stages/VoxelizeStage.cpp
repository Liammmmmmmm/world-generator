#include "wg/gen/stages/VoxelizeStage.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
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
    const Grid2D<float>& h = world.layer(layers::kHeight);
    if (h.width() != width || h.length() != length)
        throw WorldError("voxelize needs \"h\" at the block resolution: run the upscale stage first");
    const Grid2D<float>* water = world.hasLayer(layers::kWaterSurface) ? &world.layer(layers::kWaterSurface) : nullptr;
    const Grid2D<float>* kind = world.hasLayer(layers::kWater) ? &world.layer(layers::kWater) : nullptr;

    const double sea = info.seaLevelM();
    const double toBlocks = p.world.verticalExaggeration / info.blockSizeM; // vertical meters -> blocks
    const auto blockY = [&](double altitudeM) { return info.seaLevel + (altitudeM - sea) * toBlocks; };
    long belowWorld = 0, aboveWorld = 0; // columns clipped to the world height
    const auto clampY = [&](double y) {
        belowWorld += y < info.minY + 1;
        aboveWorld += y >= info.maxY;
        return std::clamp(static_cast<int>(std::floor(y)), info.minY + 1, info.maxY - 1);
    };
    const auto groundAt = [&](int x, int z) {
        return blockY(h.at(std::clamp(x, 0, width - 1), std::clamp(z, 0, length - 1)));
    };

    // Top of the ground and of the water of a column (water == ground: dry).
    struct Column {
        int ground, water;
    };
    const auto columnAt = [&](int x, int z) {
        Column c{clampY(groundAt(x, z)), 0};
        c.water = c.ground;
        if (kind && water) {
            const auto type = static_cast<int>(kind->at(x, z));
            if (type == layers::kLake || type == layers::kRiver)
                c.water = std::max(c.ground, static_cast<int>(std::floor(blockY(water->at(x, z)))));
        }
        if (c.ground < info.seaLevel) // the sea
            c.water = std::max(c.water, info.seaLevel - 1);
        return c;
    };

    VoxelVolume& voxels = world.createVoxels(Block::Air);
    const float maxGrassSlope = static_cast<float>(std::tan(v.grassMaxSlopeDeg * std::numbers::pi / 180.0));
    const int dirt = v.dirtDepthBlocks;
    const int chunk = VoxelVolume::kSectionSize;
    std::vector<Column> columns(static_cast<std::size_t>(chunk) * chunk);

    for (int cz = 0; cz < length; cz += chunk) {
        context.progress(static_cast<float>(cz) / length);
        for (int cx = 0; cx < width; cx += chunk) {
            const int x1 = std::min(cx + chunk, width), z1 = std::min(cz + chunk, length);
            // Below the lowest ground of the chunk, minus the dirt, all is
            // stone: filled as whole (uniform) sections, at no memory cost.
            int lowest = info.maxY;
            for (int z = cz; z < z1; ++z) {
                for (int x = cx; x < x1; ++x) {
                    const Column c = columnAt(x, z);
                    columns[static_cast<std::size_t>(z - cz) * chunk + (x - cx)] = c;
                    lowest = std::min(lowest, c.ground);
                }
            }
            voxels.fillBox(cx, info.minY, cz, x1, info.minY + 1, z1, Block::Bedrock);
            const int stoneTop = lowest - dirt; // exclusive
            voxels.fillBox(cx, info.minY + 1, cz, x1, stoneTop, z1, Block::Stone);

            for (int z = cz; z < z1; ++z) {
                for (int x = cx; x < x1; ++x) {
                    const Column c = columns[static_cast<std::size_t>(z - cz) * chunk + (x - cx)];
                    const int y = c.ground;
                    const double slope =
                        std::hypot(groundAt(x + 1, z) - groundAt(x - 1, z), groundAt(x, z + 1) - groundAt(x, z - 1)) / 2.0;
                    const bool wet = c.water > y; // sea, lake or river bed
                    const int from = std::max(stoneTop, info.minY + 1);
                    if (wet || slope <= maxGrassSlope) { // grass on dirt (dirt under water)
                        voxels.fillColumn(x, z, from, y - dirt, Block::Stone);
                        voxels.fillColumn(x, z, std::max(y - dirt, info.minY + 1), y, Block::Dirt);
                        voxels.set(x, y, z, wet ? Block::Dirt : Block::Grass);
                    } else { // bare rock
                        voxels.fillColumn(x, z, from, y + 1, Block::Stone);
                    }
                    if (wet)
                        voxels.fillColumn(x, z, y + 1, c.water + 1, Block::Water);
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
