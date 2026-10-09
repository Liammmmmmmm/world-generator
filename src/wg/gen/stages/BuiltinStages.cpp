// Registration of every built-in stage type. To add a stage: write a Stage
// subclass in gen/stages/, add it below, and use its type in a config.

#include "wg/gen/StageRegistry.hpp"
#include "wg/gen/stages/FlatStage.hpp"

namespace wg {

const StageRegistry& StageRegistry::builtin() {
    static const StageRegistry registry = [] {
        StageRegistry r;
        r.add<FlatStage>("flat");
        return r;
    }();
    return registry;
}

} // namespace wg
