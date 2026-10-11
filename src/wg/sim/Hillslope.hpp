#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wg {

// Sediment kinds (the "sediment" field of geology.materials).
enum SedimentKind : int { kGravel = 0, kSand = 1, kClay = 2 };
inline constexpr int kSedimentKinds = 3;
// Volumes (m of column) per kind, or fractions summing to 1.
using Composition = std::array<double, kSedimentKinds>;

// The erodible column of each cell: rock b under a sediment cover sed of
// composition frac. Material is always taken from the sediment first, then
// from the rock; what is deposited goes into the sediment.
struct Regolith {
    std::vector<double>& b;
    std::vector<double>& sed;
    std::vector<Composition>& frac;  // fractions of sed (meaningless where sed = 0)
    const std::vector<int>& rockKind; // sediment kind the surface rock breaks into

    // Removes `volume` from cell c; returns the volumes per kind removed.
    Composition remove(int c, double volume) const;
    // Adds `volumes` (per kind) to the sediment of cell c, mixing by volume.
    void deposit(int c, const Composition& volumes) const;
};

// Linear hillslope diffusion dh/dt = D lap(h) over the active cells, by the
// Peaceman-Rachford ADI scheme: a half step implicit in x (one tridiagonal
// system per row, Thomas algorithm), then a half step implicit in y.
// `kind` per cell: 1 active, 2 sink, 0 wall. Walls let no flux through; sinks
// keep their height and absorb what flows into them (added per cell to
// `absorbed`, in m of column, if given). The active cells and the sinks
// together conserve the volume.
// `r` = D dt / dx². The step is split into ceil(r) sub-steps: the explicit
// half of Peaceman-Rachford then stays monotone (no new extrema) and damps
// the grid-scale noise, which a single huge step would leave untouched.
// Returns the number of sub-steps.
int diffuseADI(std::vector<double>& h, const std::vector<std::uint8_t>& kind, int nx, int ny, double r,
               std::vector<double>* absorbed = nullptr);

// One pass of thermal collapse (Musgrave, Kolb & Mace 1989) with a double
// buffer: transfers are computed from the current heights, then applied.
// A source cell i whose slope to a lower neighbour j exceeds its talus angle
// sends `fraction` of the largest height excess (h_i - h_j - L_ij tan(talus))
// to those neighbours, in proportion to their excess. The sediment cover
// slides at the sediment angle (at most its thickness), the rock at its own
// angle; the larger of the two moves. Removed material (sediment, then rock)
// lands as sediment (scree) on the receivers (any non-fixed cell).
// Returns the volume moved.
double thermalCollapse(const Regolith& regolith, std::vector<double>& h, const std::vector<std::uint8_t>& source,
                       const std::vector<std::uint8_t>& fixed, const std::vector<double>& rockTan, double sedimentTan,
                       double fraction, int nx, int ny, double dx);

} // namespace wg
