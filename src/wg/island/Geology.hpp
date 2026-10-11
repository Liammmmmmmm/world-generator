#pragma once

#include <string>
#include <vector>

#include "wg/island/UpliftPattern.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

// The rock in place, answering "which material is at (x, y, z)".
//
// Strata are stacked from a reference surface z_ref downward (geology.strata,
// top to bottom), above the basement. The rock is displaced, and the strata
// with it, by:
//   - the cumulated tectonic uplift of the column (cum_uplift),
//   - folds: a 2D field D_fold(x, y), precomputed once (foldOffset),
//   - faults: D_fault(x, y, z), which depends on depth (dipping planes):
//     old faults (geology.faults) each raise their positive side, and the
//     fault-bounded tectonic phases (block_uplift, horst_graben) move whole
//     blocks between dipping walls, as far as the phase has gone (the walls
//     follow the warped outline of the phase's pattern, see UpliftPattern).
// so a lookup uses z_rock = z - cum_uplift - D_fold - D_fault. Intrusions
// (bell-shaped plutons of a single material) are tested first.
//
// Coordinates are meters from the map center; altitudes are absolute meters
// (sea level at the world's sea level).
class Geology {
public:
    explicit Geology(const IslandParams& params);

    // Material ids index this list (the names of geology.materials, sorted).
    const std::vector<std::string>& materialNames() const { return names_; }

    // Vertical displacement of the strata by the folds at (x, y), in meters.
    double foldOffset(double x, double y) const;
    // Vertical displacement by the faults at (x, y, z), in meters: old faults,
    // plus the blocks of the fault-bounded phases.
    double faultOffset(double x, double y, double z) const;

    // How far each tectonic phase has gone, in [0, 1]: a block_uplift or
    // horst_graben phase has moved its blocks by that share of its total uplift.
    // 0 until set (initial state: the phases have not acted yet). Step 3 sets it
    // as the simulation advances; the strata of these blocks then move with the
    // faults only, so their part of cum_uplift must not move them again.
    void setPhaseProgress(std::vector<double> progress) { phaseProgress_ = std::move(progress); }

    // Material at (x, y, z), given the column's cumulated uplift, the altitude of
    // the top of the strata pile there and the precomputed fold offset.
    int material(double x, double y, double z, double cumUplift, double zRef, double foldOffset) const;

    // Strata only (no faults, folds or intrusions): material at depth zRef - zRock.
    int strataMaterial(double zRock, double zRef) const;

private:
    struct Fault {
        double x, y;     // trace center
        double ux, uy;   // along the trace
        double nx, ny;   // horizontal normal, towards the raised side
        double halfLength;
        double cotDip;   // horizontal shift of the plane per meter of depth
        double offset;
    };
    // A block_uplift or horst_graben phase (its walls follow the warped pattern).
    struct FaultedPhase {
        int index;     // in tectonics.phases (progress)
        UpliftPattern pattern;
        double uplift; // total uplift U of the phase (m)
    };
    struct Intrusion {
        int material;
        double x, y, radius, top, domeK;
    };

    std::vector<std::string> names_;
    std::vector<double> bottoms_; // depth below z_ref of the bottom of each stratum
    std::vector<int> strata_;     // material id of each stratum
    int basement_ = 0;

    bool folded_ = false;
    double foldUx_ = 1.0, foldUy_ = 0.0; // fold axis
    double foldWavelength_ = 1.0, foldAmplitude_ = 0.0, foldAsymmetry_ = 0.0, foldTanPlunge_ = 0.0;

    std::vector<Fault> faults_; // old faults only (derived ones are the phases' walls)
    std::vector<FaultedPhase> faultedPhases_;
    std::vector<double> phaseProgress_;
    double faultTipTaper_ = 0.0;
    std::vector<Intrusion> intrusions_;
    double seaLevel_ = 0.0;
};

} // namespace wg
