#include "wg/erosion/ParticleErosion.hpp"

#include <algorithm>
#include <string>

namespace wg::erosion {

namespace {

bool isLand(const ErosionMap& map, int i, int j) {
    return map.at(i, j).height > 0.0f && !map.fixed(i, j);
}

} // namespace

ParticleErosion::ParticleErosion(ErosionMap& map, const ParticleErosionParams& params, std::uint64_t seed)
    : map_(map), params_(params), seed_(seed) {
    for (int j = 0; j < map.ny(); ++j)
        for (int i = 0; i < map.nx(); ++i)
            landCells_ += isLand(map, i, j);
    particlesPerStep_ = params.particlesPerStep > 0 ? params.particlesPerStep
                        : landCells_ > 0        ? std::max(1, landCells_ / kCellsPerParticle)
                                                : 0;
}

bool ParticleErosion::spawn(Rng& rng, float& x, float& y) const {
    // Rejection sampling: uniform over the land cells. Bounded, in case the
    // land has all but vanished.
    constexpr int kMaxAttempts = 1000;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const int i = rng.nextInt(0, map_.nx() - 1), j = rng.nextInt(0, map_.ny() - 1);
        const double u = rng.nextDouble(), v = rng.nextDouble(); // always drawn: fixed stream layout
        if (!isLand(map_, i, j))
            continue;
        x = static_cast<float>(i + u);
        y = static_cast<float>(j + v);
        return true;
    }
    return false;
}

void ParticleErosion::step(int index) {
    Rng rng(deriveSeed(seed_, "step:" + std::to_string(index)));
    for (int p = 0; p < particlesPerStep_; ++p) {
        float x, y;
        if (!spawn(rng, x, y))
            break;
        lost_ += descend(map_, params_.drop, x, y);
    }

    const float rate = params_.dischargeLrate;
    for (std::size_t k = 0; k < map_.size(); ++k) {
        ErosionCell& cell = map_[k];
        cell.discharge = (1.0f - rate) * cell.discharge + rate * cell.dischargeTrack;
        cell.momentumX = (1.0f - rate) * cell.momentumX + rate * cell.momentumXTrack;
        cell.momentumY = (1.0f - rate) * cell.momentumY + rate * cell.momentumYTrack;
        cell.dischargeTrack = cell.momentumXTrack = cell.momentumYTrack = 0.0f;
    }
}

double ParticleErosion::totalMass() const {
    double sum = lost_;
    for (std::size_t k = 0; k < map_.size(); ++k)
        sum += map_[k].height;
    return sum;
}

} // namespace wg::erosion
