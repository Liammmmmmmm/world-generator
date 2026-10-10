#pragma once

#include <cstdint>

namespace wg {

// Immutable dimensions and global settings of a world.
struct WorldInfo {
    int width = 0;  // X extent, in blocks
    int length = 0; // Z extent, in blocks
    int minY = 0;
    int maxY = 0; // exclusive
    int seaLevel = 0;
    std::uint64_t seed = 0;
    double blockSizeM = 1.0; // meters per block

    int height() const { return maxY - minY; }
    double widthM() const { return width * blockSizeM; }
    double lengthM() const { return length * blockSizeM; }
    double seaLevelM() const { return seaLevel * blockSizeM; }
};

} // namespace wg
