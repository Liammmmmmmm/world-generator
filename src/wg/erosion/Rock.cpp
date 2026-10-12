#include "wg/erosion/Rock.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wg::erosion {

Rock::Rock(const Geology& geology, const std::map<std::string, IslandParams::Material>& materials, const Frame& frame,
           double hardnessInfluence, double sedimentTalusAngleDeg, double underwaterTalusAngleDeg)
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
    underwaterTalus_ = slope(underwaterTalusAngleDeg);
    minTalus_ = std::min({sedimentTalus_, underwaterTalus_, *std::min_element(talus_.begin(), talus_.end())});

    anchor_.resize(static_cast<std::size_t>(frame.nx) * frame.ny);
    for (int j = 0; j < frame.ny; ++j)
        for (int i = 0; i < frame.nx; ++i)
            anchor_[static_cast<std::size_t>(j) * frame.nx + i] =
                geology.strataAnchor((i + 0.5 - frame.nx * 0.5) * frame.cellM, (j + 0.5 - frame.ny * 0.5) * frame.cellM);
}

int Rock::material(int i, int j, const ErosionCell& cell) const {
    const double rockTop = frame_.seaLevelM + (cell.height - cell.sediment) * frame_.heightScaleM;
    return geology_.strataMaterial(rockTop, anchor_[static_cast<std::size_t>(j) * frame_.nx + i]);
}

} // namespace wg::erosion
