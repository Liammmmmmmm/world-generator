#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "wg/core/WorldState.hpp"
#include "wg/gen/Stage.hpp"

namespace wg {

struct WorldConfig;
class StageRegistry;

class GenerationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown out of Pipeline::run() when PipelineCallbacks::shouldCancel says so.
class GenerationCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "generation cancelled"; }
};

// World state produced by a stage (final, or a preview while it runs).
// Cheap to copy and keep around (copy-on-write).
struct Snapshot {
    std::string stageName;
    std::string stageType;
    WorldState state;
    double seconds = 0.0;                   // stage duration (so far, for a preview)
    std::vector<std::string> changedLayers; // created or modified by the stage
    bool voxelsChanged = false;
    bool paramsChanged = false;
    int step = -1;      // iterative stage: steps done (-1: not iterative)
    int totalSteps = 0;
};

struct StageDescription {
    std::string name;
    std::string type;
};

// Hooks to follow a generation, e.g. from another thread. All are optional and
// are called on the thread running the pipeline.
struct PipelineCallbacks {
    std::function<void(std::size_t index)> onStageStart;
    // Live state of the running stage, sent when the stage reports progress,
    // at most once every `previewInterval` seconds.
    std::function<void(std::size_t index, Snapshot preview, float progress)> onStageProgress;
    std::function<void(std::size_t index, const Snapshot& result)> onStageDone;
    // Polled between stages and on every progress report.
    std::function<bool()> shouldCancel;
    // Called before each step of an iterative stage (StageContext::step); may
    // block to pause it. Returns true to get a preview of this very step.
    std::function<bool()> stepGate;
    // Diagnostic messages of the stages.
    std::function<void(std::size_t index, const std::string& message)> onLog;
    double previewInterval = 0.1;
};

class Pipeline {
public:
    // Builds the pipeline from config.raw["pipeline"], an array of objects
    // { "type": <stage type>, "name": <optional unique name>, ...params }, or
    // the default island pipeline (StageRegistry::defaultPipeline) if absent.
    static Pipeline fromConfig(const WorldConfig& config);
    static Pipeline fromConfig(const WorldConfig& config, const StageRegistry& registry);

    Pipeline& add(std::string name, std::string type, std::unique_ptr<Stage> stage);

    // Runs all stages and returns one snapshot per stage; the last one is the
    // final world. Throws GenerationError if a stage fails, GenerationCancelled
    // if cancelled.
    std::vector<Snapshot> run(const WorldConfig& config, const PipelineCallbacks& callbacks = {}) const;

    std::size_t size() const { return entries_.size(); }
    std::vector<StageDescription> stages() const;

private:
    struct Entry {
        std::string name;
        std::string type;
        std::unique_ptr<Stage> stage;
    };
    std::vector<Entry> entries_;
};

} // namespace wg
