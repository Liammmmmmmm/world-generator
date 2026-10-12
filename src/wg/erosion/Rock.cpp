#include "wg/erosion/Rock.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wg::erosion {

namespace {

// Below this thickness, a cell counts as bare rock for its talus angle.
constexpr double kBareRockSedimentM = 0.05;

} // namespace

Rock::Rock(const Geology& geology, const std::map<std::string, IslandParams::Material>& materials, const Frame& frame,
           double hardnessInfluence, double sedimentTalusAngleDeg)
    : geology_(geology), frame_(frame) {
    // tan(angle) is a height per meter; per cell in normalized heights:
    const auto slope = [&](double angleDeg) {
        return static_cast<float>(std::tan(angleDeg * std::numbers::pi / 180.0) * frame.cellM / frame.heightScaleM);
    };
    for (const std::string& name : geology.materialNames()) {
        const auto& m = materials.at(name);
        erodibility_.push_back(static_cast<float>(std::pow(m.erodibility, hardnessInfluence)));
        talus_.push_back(slope(m.talusAngleDeg));
    }
    sedimentTalus_ = slope(sedimentTalusAngleDeg);
    minTalus_ = std::min(sedimentTalus_, *std::min_element(talus_.begin(), talus_.end()));
}

int Rock::material(int i, int j, const ErosionCell& cell) const {
    const double x = (i + 0.5 - frame_.nx * 0.5) * frame_.cellM;
    const double y = (j + 0.5 - frame_.ny * 0.5) * frame_.cellM;
    const double rockTop = frame_.seaLevelM + (cell.height - cell.sediment) * frame_.heightScaleM;
    return geology_.material(x, y, rockTop);
}

float Rock::talus(int i, int j, const ErosionCell& cell) const {
    if (cell.sediment * frame_.heightScaleM > kBareRockSedimentM)
        return sedimentTalus_;
    return talus_[material(i, j, cell)];
}

} // namespace wg::erosion
