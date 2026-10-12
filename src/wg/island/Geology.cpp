#include "wg/island/Geology.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace wg {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

} // namespace

Geology::Geology(const IslandParams& params, const Grid2D<float>* initialSurface) {
    const auto& g = params.geology;
    for (const auto& [name, _] : g.materials)
        names_.push_back(name);
    const auto id = [&](const std::string& name) {
        return static_cast<int>(std::lower_bound(names_.begin(), names_.end(), name) - names_.begin());
    };

    double depth = 0.0;
    for (const auto& stratum : g.strata) {
        depth += stratum.thicknessM;
        bottoms_.push_back(depth);
        strata_.push_back(id(stratum.material));
    }
    basement_ = id(g.basementMaterial);

    strataTop_ = params.world.seaLevelY * params.world.blockSizeM + g.strataTopM;
    if (params.origin.type == "volcanic")
        initialSurface_ = initialSurface;
    widthM_ = params.world.width * params.world.blockSizeM;
    lengthM_ = params.world.length * params.world.blockSizeM;

    const auto& fold = g.folding;
    folded_ = fold.enabled && fold.wavelengthM > 0.0;
    foldUx_ = std::cos(fold.axisAngleDeg * kDegToRad);
    foldUy_ = std::sin(fold.axisAngleDeg * kDegToRad);
    foldWavelength_ = fold.wavelengthM;
    foldAmplitude_ = fold.amplitudeM;
    foldAsymmetry_ = fold.asymmetry;
    foldTanPlunge_ = std::tan(fold.plungeDeg * kDegToRad);
}

double Geology::foldOffset(double x, double y) const {
    if (!folded_)
        return 0.0;
    // Fold axis along u; phase across it (v), skewed by the asymmetry
    // (overturned folds), plus the plunge along the axis.
    const double u = x * foldUx_ + y * foldUy_;
    const double v = -x * foldUy_ + y * foldUx_;
    const double phi = 2.0 * std::numbers::pi * v / foldWavelength_;
    return foldAmplitude_ * std::sin(phi + foldAsymmetry_ * std::sin(phi)) + foldTanPlunge_ * u;
}

int Geology::strataMaterial(double zRock, double zRef) const {
    const double depth = zRef - zRock;
    if (strata_.empty())
        return basement_;
    if (depth < 0.0)
        return strata_.front(); // above the pile: its top layer
    const auto i = static_cast<std::size_t>(std::distance(
        bottoms_.begin(), std::upper_bound(bottoms_.begin(), bottoms_.end(), depth)));
    return i < strata_.size() ? strata_[i] : basement_;
}

double Geology::strataAnchor(double x, double y) const {
    const double zRef = initialSurface_ ? sampleCentered(*initialSurface_, x, y, widthM_, lengthM_) : strataTop_;
    return zRef + foldOffset(x, y); // the strata move with the folds
}

int Geology::material(double x, double y, double z) const {
    return strataMaterial(z, strataAnchor(x, y));
}

} // namespace wg
