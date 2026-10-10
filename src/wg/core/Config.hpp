#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wg/core/WorldInfo.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A loaded config: the seed and every generation parameter, resolved (drawn
// from the seed, then overridden by the non-null values of the file).
struct WorldConfig {
    std::uint64_t seed = 0;
    nlohmann::json raw; // whole document, as given
    std::shared_ptr<const ResolvedParams> params;

    const IslandParams& island() const { return params->values; }
    WorldInfo info() const;

    static WorldConfig fromJson(const nlohmann::json& json);
    static WorldConfig load(const std::filesystem::path& path);
};

} // namespace wg
