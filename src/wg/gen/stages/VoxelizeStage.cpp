#include "wg/gen/stages/VoxelizeStage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "wg/core/WorldState.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

namespace {

// Lakes are drawn where the interpolated lake depth passes this (m).
constexpr double kLakeMinDepthM = 0.25;

constexpr float kNoRiver = std::numeric_limits<float>::infinity();

} // namespace

void VoxelizeStage::run(WorldState& world, const StageContext& context) {
    const IslandParams& p = world.params().values;
    const auto& v = p.voxelization;
    if (!v.enabled)
        return;

    const WorldInfo& info = context.info;
    const int width = info.width, length = info.length;
    const double sea = info.seaLevelM();
    const double toBlocks = p.world.verticalExaggeration / info.blockSizeM; // vertical meters -> blocks
    const auto blockY = [&](double altitudeM) { return info.seaLevel + (altitudeM - sea) * toBlocks; };
    const auto xM = [&](int x) { return (x + 0.5) * info.blockSizeM - info.widthM() / 2.0; };
    const auto zM = [&](int z) { return (z + 0.5) * info.blockSizeM - info.lengthM() / 2.0; };
    const auto column = [&](int x, int z) { return static_cast<std::size_t>(z) * width + x; };
    const Grid2D<float>& h = world.layer(layers::kHeight);

    // Surface altitude of every block column, in blocks (fractional).
    std::vector<float> top(static_cast<std::size_t>(width) * length);
    for (int z = 0; z < length; ++z) {
        if (z % 64 == 0)
            context.progress(0.2f * static_cast<float>(z) / length);
        for (int x = 0; x < width; ++x)
            top[column(x, z)] = static_cast<float>(blockY(sampleCubic(h, xM(x), zM(z), info.widthM(), info.lengthM())));
    }
    const auto topAt = [&](int x, int z) { return top[column(std::clamp(x, 0, width - 1), std::clamp(z, 0, length - 1))]; };

    // Rivers (hydrology): each river cell is a band from its center to its
    // receiver's, as wide as the river, whose water surface goes linearly
    // from one water level to the other. Where bands overlap, the lowest
    // surface and the deepest bed win.
    const bool hydrology = world.hasLayer(layers::kRiverWidth) && world.hasByteLayer(layers::kFlowDirection);
    std::vector<float> riverLevel, riverDepth;
    if (hydrology) {
        riverLevel.assign(top.size(), kNoRiver);
        riverDepth.assign(top.size(), 0.0f);
        const Grid2D<float>& riverWidth = world.layer(layers::kRiverWidth);
        const Grid2D<float>& depthM = world.layer(layers::kRiverDepth);
        const Grid2D<float>& surface = world.layer(layers::kWaterSurface);
        const Grid2D<std::uint8_t>& direction = world.byteLayer(layers::kFlowDirection);
        const double cellBlocks = static_cast<double>(width) / riverWidth.width(); // blocks per cell
        for (int j = 0; j < riverWidth.length(); ++j) {
            for (int i = 0; i < riverWidth.width(); ++i) {
                const std::uint8_t d = direction.at(i, j);
                if (riverWidth.at(i, j) <= 0.0f || d == layers::kNoFlow)
                    continue;
                const int ri = i + layers::kFlowDi[d], rj = j + layers::kFlowDj[d];
                const double ax = (i + 0.5) * cellBlocks, az = (j + 0.5) * cellBlocks;
                const double bx = (ri + 0.5) * cellBlocks, bz = (rj + 0.5) * cellBlocks;
                const double levelA = blockY(surface.at(i, j)), levelB = blockY(surface.at(ri, rj));
                const double radius = std::max(0.75, riverWidth.at(i, j) / info.blockSizeM / 2.0);
                const float depth = static_cast<float>(depthM.at(i, j) * toBlocks);
                const double lx = bx - ax, lz = bz - az, length2 = lx * lx + lz * lz;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - radius)));
                const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max(ax, bx) + radius)));
                const int z0 = std::max(0, static_cast<int>(std::floor(std::min(az, bz) - radius)));
                const int z1 = std::min(length - 1, static_cast<int>(std::ceil(std::max(az, bz) + radius)));
                for (int z = z0; z <= z1; ++z) {
                    for (int x = x0; x <= x1; ++x) {
                        const double px = x + 0.5 - ax, pz = z + 0.5 - az;
                        const double t = std::clamp((px * lx + pz * lz) / length2, 0.0, 1.0);
                        if (std::hypot(px - t * lx, pz - t * lz) > radius)
                            continue;
                        const std::size_t c = column(x, z);
                        riverLevel[c] = std::min(riverLevel[c], static_cast<float>(levelA + (levelB - levelA) * t));
                        riverDepth[c] = std::max(riverDepth[c], depth);
                    }
                }
            }
        }
    }
    context.progress(0.3f);
    const Grid2D<float>* lakeDepth = hydrology ? &world.layer(layers::kLakeDepth) : nullptr;
    const Grid2D<float>* waterSurface = hydrology ? &world.layer(layers::kWaterSurface) : nullptr;

    // Top of the ground (river beds carved) and of the water of a column.
    struct Column {
        int ground, water; // water == ground: dry
    };
    long belowWorld = 0, aboveWorld = 0; // columns clipped to the world height
    const auto columnAt = [&](int x, int z, bool count) {
        const float t = topAt(x, z);
        if (count) {
            belowWorld += t < info.minY + 1;
            aboveWorld += t >= info.maxY;
        }
        const auto clampY = [&](double y) { return std::clamp(static_cast<int>(std::floor(y)), info.minY + 1, info.maxY - 1); };
        Column c{clampY(t), clampY(t)};
        if (hydrology) {
            const std::size_t k = column(x, z);
            if (riverLevel[k] != kNoRiver) { // carve the bed, water up to the river surface
                const int surface = clampY(riverLevel[k]);
                c.ground = std::min(c.ground, clampY(surface - std::max(1.0f, std::round(riverDepth[k]))));
                c.water = std::max(c.water, surface);
            }
            if (sampleCentered(*lakeDepth, xM(x), zM(z), info.widthM(), info.lengthM()) >= kLakeMinDepthM)
                c.water = std::max(c.water, clampY(blockY(sampleCentered(*waterSurface, xM(x), zM(z), info.widthM(), info.lengthM()))));
        }
        if (c.ground < info.seaLevel) // the sea
            c.water = std::max(c.water, info.seaLevel - 1);
        c.water = std::max(c.water, c.ground);
        return c;
    };

    VoxelVolume& voxels = world.createVoxels(Block::Air);
    const float maxGrassSlope = static_cast<float>(std::tan(v.grassMaxSlopeDeg * std::numbers::pi / 180.0));
    const int dirt = v.dirtDepthBlocks;
    const int chunk = VoxelVolume::kSectionSize;
    std::vector<Column> columns(static_cast<std::size_t>(chunk) * chunk);

    for (int cz = 0; cz < length; cz += chunk) {
        context.progress(0.3f + 0.65f * static_cast<float>(cz) / length);
        for (int cx = 0; cx < width; cx += chunk) {
            const int x1 = std::min(cx + chunk, width), z1 = std::min(cz + chunk, length);
            // Below the lowest ground of the chunk, minus the dirt, all is
            // stone: filled as whole (uniform) sections, at no memory cost.
            int lowest = info.maxY;
            for (int z = cz; z < z1; ++z) {
                for (int x = cx; x < x1; ++x) {
                    const Column c = columnAt(x, z, true);
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
                    const float slope = std::hypot(topAt(x + 1, z) - topAt(x - 1, z), topAt(x, z + 1) - topAt(x, z - 1)) / 2.0f;
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
