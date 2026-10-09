#pragma once

#include <cstdint>
#include <string_view>

#include "wg/core/WorldInfo.hpp"

namespace wg {

class WorldState;

struct StageContext {
    const WorldInfo& info;
    std::uint64_t seed;    // derived from the world seed and the stage name
    std::string_view name; // unique stage name in the pipeline
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
