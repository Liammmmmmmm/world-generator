#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

#include "wg/core/WorldState.hpp"

namespace wg {

class MinecraftExportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct MinecraftExportOptions {
    std::string levelName = "Island";
    // Data version written in the chunks and level.dat: 3955 is Minecraft
    // 1.21.1. Newer versions upgrade the world when opening it; older ones
    // refuse it.
    int dataVersion = 3955;
    std::string versionName = "1.21.1";
    int threads = 0; // regions compressed in parallel; 0: all cores
};

struct MinecraftExportReport {
    int chunks = 0;
    int regions = 0;
    int spawnX = 0, spawnY = 0, spawnZ = 0;
    int sectionsDropped = 0; // outside the Minecraft overworld (y -64..319)
};

// Writes the voxels of `world` as a Minecraft Java Edition world (Anvil
// format, 1.18+ chunks) into `directory`: level.dat (creative mode, commands
// allowed, an empty void world around the map) and region/r.X.Z.mca. The map
// is centered on the origin; the spawn is on the land closest to the center.
// Block mapping: see minecraftName() in MinecraftExport.cpp.
MinecraftExportReport exportMinecraft(const WorldState& world, const std::filesystem::path& directory,
                                      const MinecraftExportOptions& options = {},
                                      const std::function<void(float)>& progress = {});

} // namespace wg
