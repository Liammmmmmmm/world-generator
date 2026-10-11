#include "wg/sim/TimeParams.hpp"

#include <algorithm>

namespace wg {

double seaLevelOffsetM(const std::vector<IslandParams::SeaLevelPoint>& curve, double t) {
    if (curve.empty())
        return 0.0;
    if (t <= curve.front().tYears)
        return curve.front().levelM;
    for (std::size_t k = 1; k < curve.size(); ++k) {
        if (t <= curve[k].tYears) {
            const auto& a = curve[k - 1];
            const auto& b = curve[k];
            const double span = b.tYears - a.tYears;
            return span > 0.0 ? a.levelM + (b.levelM - a.levelM) * (t - a.tYears) / span : b.levelM;
        }
    }
    return curve.back().levelM;
}

double phaseActivity(const IslandParams::TectonicPhase& phase, double t) {
    const double duration = phase.endYears - phase.startYears;
    if (duration <= 0.0 || t <= phase.startYears || t >= phase.endYears)
        return 0.0;
    const double ramp = 0.05 * duration;
    return std::min({1.0, (t - phase.startYears) / ramp, (phase.endYears - t) / ramp});
}

} // namespace wg
