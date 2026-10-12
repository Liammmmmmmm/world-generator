#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "wg/params/IslandParams.hpp"

namespace wg {

// Draws parameters from a seed and applies config overrides.
//
// Every random parameter has its own random stream, derived from the seed and
// its path. The value is always drawn first and only then replaced by the
// config value (if present and not null), so overriding or adding a parameter
// never changes the value of any other one.
//
// Paths use dots and indices: "geology.strata[2].thickness_m".
class ParamResolver {
public:
    using Weights = std::vector<std::pair<std::string, double>>;

    ParamResolver(std::uint64_t seed, const nlohmann::json& config) : seed_(seed), config_(config) {}

    // --- Random parameters ---------------------------------------------------
    double uniform(const std::string& path, double min, double max);
    double logUniform(const std::string& path, double min, double max);
    int integer(const std::string& path, int min, int max); // inclusive
    bool chance(const std::string& path, double probability);
    // Weighted choice. A config value may be any of `allowed` (defaults to the
    // weighted options), so it can pick an option the draw never would.
    std::string pick(const std::string& path, const Weights& weights, const std::vector<std::string>& allowed = {});
    // Custom distribution: `f` maps a uniform value in [0, 1) to the parameter.
    double sample(const std::string& path, const std::function<double(double)>& f);
    int sampleInteger(const std::string& path, const std::function<int(double)>& f);
    // Point: `f` maps two uniform values in [0, 1) to it; config form is [x, y].
    IslandParams::Vec2 point(const std::string& path, const std::function<IslandParams::Vec2(double, double)>& f);

    // --- Fixed defaults (no randomness) --------------------------------------
    double number(const std::string& path, double fallback);
    int whole(const std::string& path, int fallback);
    bool flag(const std::string& path, bool fallback);
    // Default object deep-merged with the config object.
    nlohmann::json object(const std::string& path, const nlohmann::json& defaults);

    // --- Lists ---------------------------------------------------------------
    // Number of elements of the list at `listPath`: the length of the config
    // array if given, otherwise the `countPath` parameter (drawn in
    // [minCount, maxCount] unless set). Fields of elements are then resolved
    // like any parameter ("list[i].field"), so a config element may set only
    // some fields.
    int listSize(const std::string& listPath, const std::string& countPath, int minCount, int maxCount);

    // --- Bookkeeping ---------------------------------------------------------
    // Records a value computed outside of the helpers above.
    void record(const std::string& path, nlohmann::json value, ParamSource source);
    void markConsumed(const std::string& path) { consumed_.insert(path); }
    // Throws ConfigError if the config contains keys no parameter read.
    void checkNoUnknownKeys() const;
    std::vector<ParamEntry> takeEntries() { return std::move(entries_); }

    // Uniform [0, 1) value of the parameter's random stream.
    double unit(const std::string& path, int index = 0) const;

private:
    // Config value at `path`, or nullptr if absent or null. Marks it consumed.
    const nlohmann::json* configValue(const std::string& path);
    std::optional<double> overrideNumber(const std::string& path, bool integer);
    void unknownKeys(const nlohmann::json& node, const std::string& path, std::vector<std::string>& out) const;

    std::uint64_t seed_;
    const nlohmann::json& config_;
    std::set<std::string> consumed_;
    std::vector<ParamEntry> entries_;
};

} // namespace wg
