#include "wg/core/Config.hpp"

#include <fstream>

#include "wg/core/Random.hpp"

namespace wg {

namespace {

int readInt(const nlohmann::json& json, const char* key, int fallback, bool required) {
    if (!json.contains(key)) {
        if (required)
            throw ConfigError(std::string("missing required key '") + key + "'");
        return fallback;
    }
    const auto& value = json.at(key);
    if (!value.is_number_integer())
        throw ConfigError(std::string("'") + key + "' must be an integer");
    return value.get<int>();
}

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

WorldConfig WorldConfig::fromJson(const nlohmann::json& json) {
    if (!json.is_object())
        throw ConfigError("config root must be a JSON object");

    WorldConfig config;
    config.raw = json;
    config.seed = readSeed(json);
    config.width = readInt(json, "width", 0, true);
    config.length = readInt(json, "length", 0, true);
    config.minY = readInt(json, "minY", config.minY, false);
    config.maxY = readInt(json, "maxY", config.maxY, false);
    config.seaLevel = readInt(json, "seaLevel", config.seaLevel, false);

    if (config.width <= 0 || config.length <= 0)
        throw ConfigError("'width' and 'length' must be positive");
    if (config.minY >= config.maxY)
        throw ConfigError("'minY' must be lower than 'maxY'");
    if (config.seaLevel < config.minY || config.seaLevel >= config.maxY)
        throw ConfigError("'seaLevel' must be within [minY, maxY)");
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
