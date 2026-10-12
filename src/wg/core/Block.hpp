#pragma once

#include <cstdint>
#include <string_view>

namespace wg {

// Engine-agnostic block palette. Exporters map these to their own format
// (e.g. Block::Grass -> "minecraft:grass_block").
// Values are serialized: append new blocks, never reorder.
enum class Block : std::uint8_t {
    Air,
    Bedrock,
    Stone,
    Dirt,
    Grass,
    Sand,
    Gravel,
    Snow,
    Water,
    River, // marker on the surface of a river bed (for now; Minecraft: blue wool)
    Count
};

struct Rgb {
    std::uint8_t r, g, b;
};

struct BlockInfo {
    std::string_view name;
    Rgb color; // preview color for map rendering
};

const BlockInfo& blockInfo(Block block);

} // namespace wg
