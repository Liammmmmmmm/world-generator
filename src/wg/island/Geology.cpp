#include "wg/island/Geology.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace wg {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

double smoothstep(double edge0, double edge1, double x) {
    if (edge0 == edge1)
        return x < edge0 ? 0.0 : 1.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

Geology::Geology(const IslandParams& params) {
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
    seaLevel_ = params.world.seaLevelY * params.world.blockSizeM;

    const auto& fold = g.folding;
    folded_ = fold.enabled && fold.wavelengthM > 0.0;
    foldUx_ = std::cos(fold.axisAngleDeg * kDegToRad);
    foldUy_ = std::sin(fold.axisAngleDeg * kDegToRad);
    foldWavelength_ = fold.wavelengthM;
    foldAmplitude_ = fold.amplitudeM;
    foldAsymmetry_ = fold.asymmetry;
    foldTanPlunge_ = std::tan(fold.plungeDeg * kDegToRad);

    const auto cotDip = [](double dipDeg) { return 1.0 / std::tan(std::clamp(dipDeg, 1.0, 90.0) * kDegToRad); };
    for (const auto& f : g.faults) {
        if (f.phase >= 0)
            continue; // a wall of a faulted phase, handled with its phase
        const double a = f.angleDeg * kDegToRad;
        faults_.push_back({f.x, f.y, std::cos(a), std::sin(a), -std::sin(a), std::cos(a), f.lengthM / 2.0,
                           cotDip(f.dipDeg), f.offsetM});
    }
    const auto& phases = params.tectonics.phases;
    for (std::size_t k = 0; k < phases.size(); ++k) {
        const auto& p = phases[k];
        if (p.type != "block_uplift" && p.type != "horst_graben")
            continue;
        const double a = p.angleDeg * kDegToRad;
        faultedPhases_.push_back({static_cast<int>(k), p.type == "horst_graben", p.center.x, p.center.y, std::cos(a),
                                  std::sin(a), p.lengthM, p.widthM, p.falloff, cotDip(p.faultDipDeg),
                                  p.rateMPerYear * (p.endYears - p.startYears)});
    }
    faultTipTaper_ = g.faultTipTaper;

    for (const auto& i : g.intrusions)
        intrusions_.push_back({id(i.material), i.x, i.y, i.radiusM, seaLevel_ + i.topM, i.domeK});
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

double Geology::faultOffset(double x, double y, double z) const {
    double offset = 0.0;
    for (const Fault& f : faults_) {
        const double dx = x - f.x, dy = y - f.y;
        const double along = dx * f.ux + dy * f.uy;
        if (std::abs(along) > f.halfLength)
            continue;
        // The plane contains the trace at sea level and dips towards the lowered
        // side: at depth d below sea level it lies d * cot(dip) on that side. The
        // raised side (footwall) is where the horizontal distance to it is > 0.
        const double side = dx * f.nx + dy * f.ny + (seaLevel_ - z) * f.cotDip;
        if (side <= 0.0)
            continue;
        const double tip = 1.0 - smoothstep(1.0 - faultTipTaper_, 1.0, std::abs(along) / f.halfLength);
        offset += f.offset * tip;
    }

    // Fault-bounded phases: blocks between walls through their surface outline
    // at sea level, dipping away from the raised block (normal faults: a horst
    // widens with depth). Shift of the walls at this altitude:
    for (const FaultedPhase& p : faultedPhases_) {
        const double share = static_cast<std::size_t>(p.index) < phaseProgress_.size() ? phaseProgress_[p.index] : 0.0;
        if (share == 0.0)
            continue;
        const double widen = (seaLevel_ - z) * p.cotDip; // > 0 below sea level
        const double dx = x - p.cx, dy = y - p.cy;
        const double u = dx * p.ux + dy * p.uy, v = -dx * p.uy + dy * p.ux;
        if (!p.horstGraben) {
            if (std::abs(u) < p.length / 2 + widen && std::abs(v) < p.width / 2 + widen)
                offset += share * p.uplift;
            continue;
        }
        // Bands of width w across the axis: horsts (even bands) raised by U,
        // grabens lowered by U; each horst widens by `widen` on both sides.
        if (std::abs(u) > p.length / 2 + widen)
            continue;
        const double band = std::floor(v / p.width);
        const double inBand = v - band * p.width; // [0, w)
        const bool evenBand = std::fmod(std::abs(band), 2.0) == 0.0;
        const bool nearEdge = inBand < std::abs(widen) || inBand > p.width - std::abs(widen);
        // Below sea level a graben edge belongs to the neighbouring horst; above, a horst edge to the graben.
        const bool horst = evenBand ? !(widen < 0.0 && nearEdge) : (widen > 0.0 && nearEdge);
        offset += share * (horst ? p.uplift : -p.uplift);
    }
    return offset;
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

int Geology::material(double x, double y, double z, double cumUplift, double zRef, double foldOffset) const {
    const double zRock = z - cumUplift - foldOffset - faultOffset(x, y, z); // the strata move with the rock
    // Intrusions first: domes whose top drops by dome_k * radius at their edge.
    for (const Intrusion& in : intrusions_) {
        const double d = std::hypot(x - in.x, y - in.y);
        if (d < in.radius && zRock < in.top - in.domeK * (d / in.radius) * (d / in.radius) * in.radius)
            return in.material;
    }
    return strataMaterial(zRock, zRef);
}

} // namespace wg
