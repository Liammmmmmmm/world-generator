#pragma once

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

// World state right after a stage ran. Cheap to keep around (copy-on-write).
struct Snapshot {
    std::string stageName;
    std::string stageType;
    WorldState state;
    double seconds = 0.0;
    std::vector<std::string> changedLayers; // created or modified by the stage
    bool voxelsChanged = false;
};

class Pipeline {
public:
    using StageCallback = std::function<void(const Snapshot&)>;

    // Builds the pipeline from config.raw["pipeline"], an array of objects
    // { "type": <stage type>, "name": <optional unique name>, ...params }.
    static Pipeline fromConfig(const WorldConfig& config);
    static Pipeline fromConfig(const WorldConfig& config, const StageRegistry& registry);

    Pipeline& add(std::string name, std::string type, std::unique_ptr<Stage> stage);

    // Runs all stages and returns one snapshot per stage; the last one is the
    // final world. Throws GenerationError if a stage fails.
    std::vector<Snapshot> run(const WorldInfo& info, const StageCallback& onStageDone = {}) const;

    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        std::string type;
        std::unique_ptr<Stage> stage;
    };
    std::vector<Entry> entries_;
};

} // namespace wg
