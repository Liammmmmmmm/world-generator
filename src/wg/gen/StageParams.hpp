#pragma once

#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "wg/core/Config.hpp"

namespace wg {

// Typed, validated access to a stage's JSON object. Every key read is
// recorded so unknown keys (typos) can be reported as errors.
class StageParams {
public:
    explicit StageParams(const nlohmann::json& json) : json_(json) {}

    template <typename T>
    T get(std::string_view key, const T& fallback) {
        const auto* value = find(key);
        return value ? convert<T>(key, *value) : fallback;
    }

    template <typename T>
    std::optional<T> getOptional(std::string_view key) {
        const auto* value = find(key);
        return value ? std::optional<T>(convert<T>(key, *value)) : std::nullopt;
    }

    template <typename T>
    T require(std::string_view key) {
        const auto* value = find(key);
        if (!value)
            throw ConfigError("missing required parameter '" + std::string(key) + "'");
        return convert<T>(key, *value);
    }

    // String parameter restricted to a set of values.
    std::string choice(std::string_view key, std::string_view fallback, std::initializer_list<std::string_view> allowed);

    void markUsed(std::string_view key) { used_.emplace(key); }
    // Throws ConfigError if the object contains keys nobody read.
    void checkNoUnknownKeys() const;

private:
    const nlohmann::json* find(std::string_view key);

    template <typename T>
    static T convert(std::string_view key, const nlohmann::json& value) {
        bool ok;
        const char* expected;
        if constexpr (std::is_same_v<T, bool>)
            ok = value.is_boolean(), expected = "a boolean";
        else if constexpr (std::is_integral_v<T>)
            ok = value.is_number_integer(), expected = "an integer";
        else if constexpr (std::is_floating_point_v<T>)
            ok = value.is_number(), expected = "a number";
        else
            ok = value.is_string(), expected = "a string";
        if (!ok)
            throw ConfigError("parameter '" + std::string(key) + "' must be " + expected);
        return value.get<T>();
    }

    const nlohmann::json& json_;
    std::set<std::string, std::less<>> used_;
};

} // namespace wg
