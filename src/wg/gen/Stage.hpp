#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "wg/core/WorldInfo.hpp"

namespace wg {

class WorldState;
struct WorldConfig;

struct StageContext {
    const WorldConfig& config; // seed, raw document and resolved parameters
    const WorldInfo& info;
    std::uint64_t seed;    // derived from the world seed and the stage name
    std::string_view name; // unique stage name in the pipeline

    // Reports how far the stage is (0..1) and lets observers such as the viewer
    // display the world as it is right now. Rate-limited and cheap when nobody
    // listens: call it as often as convenient (e.g. once per erosion iteration).
    // Long stages should call it regularly: it is also where a cancelled
    // generation stops (by throwing GenerationCancelled).
    //  - `force` skips the rate limit (e.g. the run is paused: show every step),
    //  - `beforePreview` runs only when a preview is about to be taken: stages
    //    that work on private buffers write them to the world layers there.
    void progress(float fraction, bool force = false, const std::function<void()>& beforePreview = {}) const {
        if (onProgress)
            onProgress(fraction, force, beforePreview);
    }

    // Called once per step by iterative stages (simulations). Lets an observer
    // pause, single-step or slow down the run; may block. Returns true when the
    // run is paused or stepping, so the stage should publish every step.
    bool tick() const { return onTick ? onTick() : false; }

    // A line of log (progress figures, warnings).
    void log(const std::string& message) const {
        if (onLog)
            onLog(message);
    }

    std::function<void(float, bool, const std::function<void()>&)> onProgress;
    std::function<bool()> onTick;
    std::function<void(const std::string&)> onLog;
};

// One step of the generation pipeline (noise, erosion, voxelization, caves...).
//
// A stage reads its parameters in its constructor (see StageParams), so config
// errors are reported before anything runs. run() reads and writes the world
// state; it only needs to know the layers it consumes and produces, which keeps
// stages independent and lets them be inserted anywhere in the pipeline.
class Stage {
public:
    virtual ~Stage() = default;
    virtual void run(WorldState& world, const StageContext& context) = 0;
};

} // namespace wg
