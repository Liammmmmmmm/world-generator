#include "wg/gen/StageParams.hpp"

#include <algorithm>

namespace wg {

const nlohmann::json* StageParams::find(std::string_view key) {
    markUsed(key);
    const auto it = json_.find(std::string(key));
    return it != json_.end() ? &*it : nullptr;
}

std::string StageParams::choice(std::string_view key, std::string_view fallback,
                                std::initializer_list<std::string_view> allowed) {
    auto value = get<std::string>(key, std::string(fallback));
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
        std::string list;
        for (auto option : allowed)
            list += (list.empty() ? "'" : ", '") + std::string(option) + "'";
        throw ConfigError("parameter '" + std::string(key) + "' must be one of " + list);
    }
    return value;
}

void StageParams::checkNoUnknownKeys() const {
    for (const auto& [key, _] : json_.items())
        if (!used_.contains(key))
            throw ConfigError("unknown parameter '" + key + "'");
}

} // namespace wg
