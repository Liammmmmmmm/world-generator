#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wg/gen/Stage.hpp"
#include "wg/gen/StageParams.hpp"

namespace wg {

// Maps a stage "type" (as written in the config) to a factory.
class StageRegistry {
public:
    using Factory = std::function<std::unique_ptr<Stage>(StageParams&)>;

    void add(std::string type, Factory factory);

    template <typename S>
    void add(std::string type) {
        add(std::move(type), [](StageParams& params) { return std::make_unique<S>(params); });
    }

    std::unique_ptr<Stage> create(std::string_view type, StageParams& params) const;
    std::vector<std::string> types() const;

    // Every stage shipped with the generator (see gen/stages/BuiltinStages.cpp).
    static const StageRegistry& builtin();
    // Stage types run, in order, when the config has no "pipeline" key.
    static const std::vector<std::string>& defaultPipeline();

private:
    std::map<std::string, Factory, std::less<>> factories_;
};

} // namespace wg
