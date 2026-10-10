#include "wg/core/Config.hpp"

#include <fstream>

#include "wg/core/Random.hpp"

namespace wg {

namespace {

std::uint64_t readSeed(const nlohmann::json& json) {
    if (!json.contains("seed"))
        throw ConfigError("missing required key 'seed'");
    const auto& seed = json.at("seed");
    if (seed.is_number_unsigned())
        return seed.get<std::uint64_t>();
    if (seed.is_number_integer())
        return static_cast<std::uint64_t>(seed.get<std::int64_t>());
    if (seed.is_string()) // Minecraft-style text seeds
        return hashString(seed.get<std::string>());
    throw ConfigError("'seed' must be an integer or a string");
}

} // namespace

WorldInfo WorldConfig::info() const {
    const auto& w = island().world;
    return {w.width, w.length, w.minY, w.maxY, w.seaLevelY, seed, w.blockSizeM};
}

WorldConfig WorldConfig::fromJson(const nlohmann::json& json) {
    if (!json.is_object())
        throw ConfigError("config root must be a JSON object");

    WorldConfig config;
    config.raw = json;
    config.seed = readSeed(json);
    // "pipeline" (optional) is read by Pipeline::fromConfig.
    config.params = std::make_shared<const ResolvedParams>(resolveIslandParams(config.seed, json, {"pipeline"}));
    return config;
}

WorldConfig WorldConfig::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file)
        throw ConfigError("cannot open config file '" + path.string() + "'");
    try {
        return fromJson(nlohmann::json::parse(file, nullptr, true, /*ignore_comments=*/true));
    } catch (const nlohmann::json::parse_error& e) {
        throw ConfigError("invalid JSON in '" + path.string() + "': " + e.what());
    }
}

} // namespace wg
