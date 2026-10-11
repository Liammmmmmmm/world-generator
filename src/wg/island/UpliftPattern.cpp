#include "wg/island/UpliftPattern.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "wg/core/Random.hpp"

namespace wg {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

double smoothstep(double edge0, double edge1, double x) {
    if (edge0 == edge1)
        return x < edge0 ? 0.0 : 1.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double gauss(double t) {
    return std::exp(-0.5 * t * t);
}

// 1 in the middle of a feature, falling to 0 at its ends (|t| = 0.5, t being
// the position along the feature divided by its length), over `falloff` of
// the half-length.
double taper(double t, double falloff) {
    return 1.0 - smoothstep(0.5 * (1.0 - falloff), 0.5, std::abs(t));
}

std::uint64_t noiseSeed(const IslandParams& params, int index, const char* name) {
    return deriveSeed(params.seed, "uplift[" + std::to_string(index) + "]:" + name);
}

} // namespace

UpliftPattern::UpliftPattern(const IslandParams& params, int index)
    : warpU_(noiseSeed(params, index, "warp_u"), {params.tectonics.upliftWarpFrequency, 2, 2.0, 0.5}),
      warpV_(noiseSeed(params, index, "warp_v"), {params.tectonics.upliftWarpFrequency, 2, 2.0, 0.5}),
      modulationNoise_(noiseSeed(params, index, "modulation"),
                       {params.tectonics.upliftModulationFrequency, 3, 2.0, 0.5}) {
    const auto& phase = params.tectonics.phases.at(static_cast<std::size_t>(index));
    const std::string& t = phase.type;
    type_ = t == "subsidence"     ? Type::Subsidence
            : t == "collision"    ? Type::Collision
            : t == "block_uplift" ? Type::Block
            : t == "horst_graben" ? Type::HorstGraben
            : t == "tilt"         ? Type::Tilt
                                  : Type::Dome;
    cx_ = phase.center.x;
    cy_ = phase.center.y;
    ux_ = std::cos(phase.angleDeg * kDegToRad);
    uy_ = std::sin(phase.angleDeg * kDegToRad);
    length_ = phase.lengthM;
    width_ = phase.widthM;
    falloff_ = phase.falloff;
    negativeFactor_ = phase.negativeFactor;
    cotDip_ = 1.0 / std::tan(std::clamp(phase.faultDipDeg, 1.0, 90.0) * kDegToRad);
    radius_ = params.islandRadiusM();
    warpAmplitude_ = params.tectonics.upliftWarpAmplitude;
    modulation_ = params.tectonics.upliftModulation;
    seaLevel_ = params.world.seaLevelY * params.world.blockSizeM;
}

UpliftPattern::Point UpliftPattern::at(double x, double y) const {
    const double qx = x / radius_, qy = y / radius_;
    const double warp = radius_ * warpAmplitude_;
    const double dx = x + warp * warpU_(qx, qy) - cx_, dy = y + warp * warpV_(qx, qy) - cy_;
    // fBm stays mostly within [-0.7, 0.7]: stretched to cover [0, 1].
    const double n = std::clamp(0.5 + 0.7 * modulationNoise_(qx, qy), 0.0, 1.0);
    return {dx * ux_ + dy * uy_, -dx * uy_ + dy * ux_, 1.0 - modulation_ * (1.0 - n)};
}

double UpliftPattern::blockDistance(double u, double v) const {
    // Rounded rectangle of half-sizes (L/2, W/2), corners rounded over half the
    // smaller half-size.
    const double hx = length_ / 2.0, hy = width_ / 2.0, corner = 0.5 * std::min(hx, hy);
    const double qx = std::abs(u) - (hx - corner), qy = std::abs(v) - (hy - corner);
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - corner;
}

double UpliftPattern::value(double x, double y) const {
    const Point p = at(x, y);
    double value = 0.0;
    switch (type_) {
    case Type::Dome:
        value = gauss(std::hypot(p.u, p.v) / width_);
        break;
    case Type::Subsidence:
        value = -gauss(std::hypot(p.u, p.v) / width_);
        break;
    case Type::Collision:
        value = gauss(p.v / width_) * taper(p.u / length_, falloff_);
        break;
    case Type::Block:
        // Soft edge of falloff * W / 2 inside the outline.
        value = smoothstep(0.0, falloff_ * width_ / 2.0, -blockDistance(p.u, p.v));
        break;
    case Type::HorstGraben: {
        // Bands of width W along the axis: +1 (horst) for even floor(v / W), -1
        // (graben) for odd, switching over a narrow band around each boundary;
        // finite along the axis.
        const double band = std::floor(p.v / width_);
        const double sign = std::fmod(std::abs(band), 2.0) == 0.0 ? 1.0 : -negativeFactor_;
        const double toBoundary = std::min(p.v - band * width_, (band + 1.0) * width_ - p.v);
        value = sign * smoothstep(0.0, falloff_ * width_ / 2.0, toBoundary) * taper(p.u / length_, falloff_);
        break;
    }
    case Type::Tilt:
        value = std::clamp(p.v / width_, -1.0, 1.0);
        if (value < 0.0)
            value *= negativeFactor_;
        break;
    }
    return value * p.modulation;
}

double UpliftPattern::blockShare(double x, double y, double z) const {
    if (!faulted())
        return 0.0;
    const Point p = at(x, y);
    const double widen = (seaLevel_ - z) * cotDip_; // > 0 below sea level
    if (type_ == Type::Block)
        return blockDistance(p.u, p.v) < -falloff_ * width_ / 4.0 + widen ? p.modulation : 0.0;

    // Horsts (even bands) widen by `widen` on both sides; the walls are the band
    // boundaries at sea level (middle of the soft edges).
    if (std::abs(p.u) > length_ / 2.0 + widen)
        return 0.0;
    const double band = std::floor(p.v / width_);
    const double inBand = p.v - band * width_; // [0, W)
    const bool evenBand = std::fmod(std::abs(band), 2.0) == 0.0;
    const bool nearEdge = inBand < std::abs(widen) || inBand > width_ - std::abs(widen);
    // Below sea level a graben edge belongs to the neighbouring horst; above, a horst edge to the graben.
    const bool horst = evenBand ? !(widen < 0.0 && nearEdge) : (widen > 0.0 && nearEdge);
    return (horst ? 1.0 : -negativeFactor_) * p.modulation;
}

} // namespace wg
