#pragma once

#include "wg/noise/Fbm.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

// Spatial pattern of a tectonic phase: the shape that its rate scales.
//
// Shapes are computed in the phase frame (u along its axis, v across), at a
// domain-warped position p' = p + R * warp_amplitude * (fbm_a, fbm_b), so no
// edge or axis is ever straight. The result is then modulated by a second
// noise, lerp(1 - modulation, 1, fbm), so no uplift is ever uniform. The
// lowered part of a tilt or horst_graben is scaled by the phase's
// negative_factor.
//
// Used by the island initialization (uplift_pattern layers), the geology
// (faulted blocks move between walls that follow the same warped outline) and
// the parameter resolution (mean uplift over the island).
class UpliftPattern {
public:
    UpliftPattern(const IslandParams& params, int index);

    // Pattern at (x, y) (meters from the map center), in [-1, 1].
    double value(double x, double y) const;

    // Faulted phases (block_uplift, horst_graben): share of the phase's total
    // uplift by which the rock at (x, y, altitude z) is moved once the phase is
    // complete: the modulation in a raised block, -negative_factor times it in a
    // graben, 0 elsewhere. The walls run through the middle of the soft edges
    // at sea level and dip away from the raised blocks (normal faults: a horst
    // widens with depth). 0 for the other types.
    double blockShare(double x, double y, double z) const;

    bool faulted() const { return type_ == Type::Block || type_ == Type::HorstGraben; }

private:
    enum class Type { Dome, Subsidence, Collision, Block, HorstGraben, Tilt };
    struct Point {
        double u, v;       // warped position in the phase frame
        double modulation; // in [1 - modulation, 1]
    };
    Point at(double x, double y) const;
    // Signed distance to the block outline (rounded rectangle), < 0 inside.
    double blockDistance(double u, double v) const;

    Type type_ = Type::Dome;
    double cx_, cy_, ux_, uy_;
    double length_, width_, falloff_, negativeFactor_, cotDip_;
    double radius_, warpAmplitude_, modulation_;
    double seaLevel_;
    Fbm warpU_, warpV_, modulationNoise_;
};

} // namespace wg
