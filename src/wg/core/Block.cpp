#include "wg/core/Block.hpp"

#include <array>

namespace wg {

namespace {

constexpr std::array<BlockInfo, static_cast<std::size_t>(Block::Count)> kBlocks{{
    {"air", {0, 0, 0}},
    {"bedrock", {50, 50, 50}},
    {"stone", {125, 125, 125}},
    {"dirt", {134, 96, 67}},
    {"grass", {106, 170, 64}},
    {"sand", {219, 207, 163}},
    {"gravel", {136, 126, 126}},
    {"snow", {240, 240, 245}},
    {"water", {48, 84, 200}},
}};

} // namespace

const BlockInfo& blockInfo(Block block) {
    const auto index = static_cast<std::size_t>(block);
    return kBlocks[index < kBlocks.size() ? index : 0];
}

} // namespace wg
