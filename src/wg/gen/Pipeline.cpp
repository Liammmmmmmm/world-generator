#include "wg/gen/Pipeline.hpp"

#include <chrono>
#include <set>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"
#include "wg/gen/StageRegistry.hpp"

namespace wg {

Pipeline Pipeline::fromConfig(const WorldConfig& config) {
    return fromConfig(config, StageRegistry::builtin());
}

Pipeline Pipeline::fromConfig(const WorldConfig& config, const StageRegistry& registry) {
    const auto it = config.raw.find("pipeline");
    if (it == config.raw.end() || !it->is_array() || it->empty())
        throw ConfigError("'pipeline' must be a non-empty array of stages");

    Pipeline pipeline;
    std::set<std::string> names;
    for (std::size_t i = 0; i < it->size(); ++i) {
        const auto& json = (*it)[i];
        std::string label = "pipeline[" + std::to_string(i) + "]";
        try {
            if (!json.is_object())
                throw ConfigError("stage must be an object");
            StageParams params(json);
            auto type = params.require<std::string>("type");
            auto name = params.get<std::string>("name", type);
            label += " '" + name + "'";
            if (!names.insert(name).second)
                throw ConfigError("duplicate stage name (set a distinct \"name\")");

            auto stage = registry.create(type, params);
            params.checkNoUnknownKeys();
            pipeline.add(std::move(name), std::move(type), std::move(stage));
        } catch (const ConfigError& e) {
            throw ConfigError(label + ": " + e.what());
        }
    }
    return pipeline;
}

Pipeline& Pipeline::add(std::string name, std::string type, std::unique_ptr<Stage> stage) {
    entries_.push_back({std::move(name), std::move(type), std::move(stage)});
    return *this;
}

std::vector<Snapshot> Pipeline::run(const WorldInfo& info, const StageCallback& onStageDone) const {
    std::vector<Snapshot> snapshots;
    snapshots.reserve(entries_.size());

    WorldState state(info);
    for (const auto& entry : entries_) {
        // Holding the previous state forces copy-on-write and lets us diff.
        const WorldState before = state;
        const StageContext context{info, deriveSeed(info.seed, entry.name), entry.name};

        const auto start = std::chrono::steady_clock::now();
        try {
            entry.stage->run(state, context);
        } catch (const std::exception& e) {
            throw GenerationError("stage '" + entry.name + "' failed: " + e.what());
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        Snapshot snapshot{entry.name, entry.type, state, elapsed.count(), {}, false};
        for (const auto& name : state.layerNames())
            if (state.layerIdentity(name) != before.layerIdentity(name))
                snapshot.changedLayers.push_back(name);
        snapshot.voxelsChanged = state.voxelsIdentity() != before.voxelsIdentity();

        if (onStageDone)
            onStageDone(snapshot);
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

} // namespace wg
