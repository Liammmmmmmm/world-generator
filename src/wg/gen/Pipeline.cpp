#include "wg/gen/Pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <set>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"
#include "wg/gen/StageRegistry.hpp"

namespace wg {

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Fills in what `state` changed compared to `before`.
void diff(Snapshot& snapshot, const WorldState& state, const WorldState& before) {
    for (const auto& name : state.layerNames())
        if (state.layerIdentity(name) != before.layerIdentity(name))
            snapshot.changedLayers.push_back(name);
    snapshot.voxelsChanged = state.voxelsIdentity() != before.voxelsIdentity();
    snapshot.paramsChanged = state.paramsHandle() != before.paramsHandle();
}

} // namespace

Pipeline Pipeline::fromConfig(const WorldConfig& config) {
    return fromConfig(config, StageRegistry::builtin());
}

Pipeline Pipeline::fromConfig(const WorldConfig& config, const StageRegistry& registry) {
    const auto it = config.raw.find("pipeline");
    if (it == config.raw.end()) {
        Pipeline pipeline;
        for (const auto& type : StageRegistry::defaultPipeline()) {
            StageParams params(nlohmann::json::object());
            pipeline.add(type, type, registry.create(type, params));
        }
        return pipeline;
    }
    if (!it->is_array())
        throw ConfigError("'pipeline' must be an array of stages");

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

std::vector<StageDescription> Pipeline::stages() const {
    std::vector<StageDescription> stages;
    for (const auto& entry : entries_)
        stages.push_back({entry.name, entry.type});
    return stages;
}

std::vector<Snapshot> Pipeline::run(const WorldConfig& config, const PipelineCallbacks& callbacks) const {
    const WorldInfo info = config.info();
    const auto checkCancelled = [&] {
        if (callbacks.shouldCancel && callbacks.shouldCancel())
            throw GenerationCancelled();
    };

    std::vector<Snapshot> snapshots;
    snapshots.reserve(entries_.size());

    WorldState state(info);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        checkCancelled();
        if (callbacks.onStageStart)
            callbacks.onStageStart(i);

        // Holding the previous state forces copy-on-write and lets us diff.
        const WorldState before = state;
        const auto start = Clock::now();
        auto lastPreview = start;

        const auto sendPreview = [&](float fraction, int step, int total) {
            lastPreview = Clock::now();
            Snapshot preview{entry.name, entry.type, {}, secondsSince(start), {}, false, false, step, total};
            diff(preview, state, before);
            preview.state = state.deepCopy();
            callbacks.onStageProgress(i, std::move(preview), std::clamp(fraction, 0.0f, 1.0f));
        };
        int lastStep = -1, lastTotal = 0;
        StageContext context{config, info, deriveSeed(info.seed, entry.name), entry.name, {}, {}, {}};
        context.onProgress = [&](float fraction) {
            checkCancelled();
            if (callbacks.onStageProgress && secondsSince(lastPreview) >= callbacks.previewInterval)
                sendPreview(fraction, lastStep, lastTotal);
        };
        context.onStep = [&](int done, int total, const std::function<void()>& publish) {
            checkCancelled();
            lastStep = done, lastTotal = total;
            const bool wanted = callbacks.stepGate && callbacks.stepGate();
            checkCancelled(); // the gate may have waited for a long time
            if (!callbacks.onStageProgress ||
                (!wanted && done < total && secondsSince(lastPreview) < callbacks.previewInterval))
                return;
            publish();
            sendPreview(total > 0 ? static_cast<float>(done) / total : 1.0f, done, total);
        };
        context.onLog = [&](const std::string& message) {
            if (callbacks.onLog)
                callbacks.onLog(i, message);
        };

        try {
            entry.stage->run(state, context);
        } catch (const GenerationCancelled&) {
            throw;
        } catch (const std::exception& e) {
            throw GenerationError("stage '" + entry.name + "' failed: " + e.what());
        }

        Snapshot snapshot{entry.name, entry.type, state, secondsSince(start), {}, false, false, lastStep, lastTotal};
        diff(snapshot, state, before);
        if (callbacks.onStageDone)
            callbacks.onStageDone(i, snapshot);
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

} // namespace wg
