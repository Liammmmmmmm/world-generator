#pragma once

#include <cstdint>
#include <functional>
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
    void progress(float fraction) const {
        if (onProgress)
            onProgress(fraction);
    }

    std::function<void(float)> onProgress;
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
