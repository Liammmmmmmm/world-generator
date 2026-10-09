#include "wg/gen/StageRegistry.hpp"

namespace wg {

void StageRegistry::add(std::string type, Factory factory) {
    factories_[std::move(type)] = std::move(factory);
}

std::unique_ptr<Stage> StageRegistry::create(std::string_view type, StageParams& params) const {
    const auto it = factories_.find(type);
    if (it == factories_.end()) {
        std::string known;
        for (const auto& name : types())
            known += (known.empty() ? "" : ", ") + name;
        throw ConfigError("unknown stage type '" + std::string(type) + "' (known: " + known + ")");
    }
    return it->second(params);
}

std::vector<std::string> StageRegistry::types() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : factories_)
        names.push_back(name);
    return names;
}

} // namespace wg
