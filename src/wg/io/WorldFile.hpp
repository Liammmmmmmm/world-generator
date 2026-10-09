#pragma once

#include <filesystem>
#include <stdexcept>

#include "wg/core/WorldState.hpp"

namespace wg {

class WorldFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Native binary format (.wgw): lossless save of a WorldState, read back by the
// viewer and future exporters. Little-endian, layout:
//   char[4] "WGW\0" | u32 version
//   i32 width, length, minY, maxY, seaLevel | u64 seed
//   u32 layerCount, then per layer: u32 nameLength | name | f32[width * length]
//   u8 hasVoxels, then per section: u8 0 + u8 block (uniform) | u8 1 + u8[4096]
void writeWorld(const WorldState& world, const std::filesystem::path& path);
WorldState readWorld(const std::filesystem::path& path);

} // namespace wg
