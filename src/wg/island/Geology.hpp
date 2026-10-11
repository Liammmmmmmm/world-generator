#pragma once

#include <string>
#include <vector>

#include "wg/core/Grid2D.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

// The rock in place, answering "which material is at (x, y, z)".
//
// Strata are stacked from a reference surface z_ref downward (geology.strata,
// top to bottom), above the basement. z_ref is the top of the pile
// (geology.strata_top_m, horizontal strata), or for a volcanic island the
// initial surface itself (lava and tuff layers parallel to the flanks). Folds
// displace the strata vertically by a 2D field D_fold(x, y), so a lookup uses
// the depth z_ref - (z - D_fold).
//
// Coordinates are meters from the map center; altitudes are absolute meters
// (sea level at the world's sea level).
class Geology {
public:
    // `initialSurface` (h_initial, covering the whole map) is the reference of
    // volcanic strata; it must outlive the Geology. Without it, strata are
    // horizontal whatever the origin.
    explicit Geology(const IslandParams& params, const Grid2D<float>* initialSurface = nullptr);

    // Material ids index this list (the names of geology.materials, sorted).
    const std::vector<std::string>& materialNames() const { return names_; }

    // Vertical displacement of the strata by the folds at (x, y), in meters.
    double foldOffset(double x, double y) const;

    // Material at (x, y, z).
    int material(double x, double y, double z) const;

    // Strata only (no folds): material at depth zRef - zRock.
    int strataMaterial(double zRock, double zRef) const;

private:
    std::vector<std::string> names_;
    std::vector<double> bottoms_; // depth below z_ref of the bottom of each stratum
    std::vector<int> strata_;     // material id of each stratum
    int basement_ = 0;

    double strataTop_ = 0.0;                     // absolute altitude of the top of the pile
    const Grid2D<float>* initialSurface_ = nullptr; // volcanic strata reference, or null
    double widthM_ = 0.0, lengthM_ = 0.0;

    bool folded_ = false;
    double foldUx_ = 1.0, foldUy_ = 0.0; // fold axis
    double foldWavelength_ = 1.0, foldAmplitude_ = 0.0, foldAsymmetry_ = 0.0, foldTanPlunge_ = 0.0;
};

} // namespace wg
