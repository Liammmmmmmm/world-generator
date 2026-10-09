#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wg/core/WorldInfo.hpp"

namespace wg {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct WorldConfig {
    std::uint64_t seed = 0;
    int width = 0;  // X extent, in blocks
    int length = 0; // Z extent, in blocks

    // Optional keys (defaults match a modern Minecraft overworld).
    int minY = -64;
    int maxY = 320; // exclusive
    int seaLevel = 62;

    // Whole document (the generation pipeline is read from raw["pipeline"]).
    nlohmann::json raw;

    WorldInfo info() const { return {width, length, minY, maxY, seaLevel, seed}; }

    static WorldConfig fromJson(const nlohmann::json& json);
    static WorldConfig load(const std::filesystem::path& path);
};

} // namespace wg
