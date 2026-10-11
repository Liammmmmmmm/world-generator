#pragma once

#include <cstdint>
#include <vector>

#include "wg/sim/Hillslope.hpp"
#include "wg/sim/Routing.hpp"

namespace wg {

// Stream power law with sediment deposition (Yuan, Braun, Guerit et al. 2019,
// JGR Earth Surface), n = 1:
//
//   dh/dt = U - K Q^m S + G Qs / A
//
// Qs: sediment flux entering the cell from upstream (m3/yr), A: drainage area
// (m2). Deposition is G Qs / A, the volume-per-area form of the paper's
// (G / (p A)) Qs: with a uniform rainfall p it is the "(G / Q) Qs" of the
// formulation, times p, and it stays well defined at the head of the basins
// (A >= one cell, and Qs = 0 there).
//
// Implicit in h along the stack, Gauss-Seidel on Qs (as in the paper): each
// iteration solves h from the base level up with the Qs of the previous one,
// then recomputes Qs down the reversed stack from the resulting erosion and
// deposition. It stops when the relative change of Qs (L1) falls below the
// tolerance, or after maxIterations. With G = 0 one pass gives exactly the
// erosion-only scheme of part 1 (Qs no longer feeds back).
//
// Within a cell, the deposit joins the sediment cover first; erosion then
// takes the sediment at k_sediment, and cuts into the rock at the rock's K
// for the rest of the step (the time split of part 2).
//
// Sinks of the flux: lakes (lake cells keep every incoming grain until the
// lake is full: the flux crosses the lake along the receivers, filling it
// from where it enters), the sea (each river mouth builds a delta: its load
// fills the sea up to sea level, outwards from the mouth, then the marine
// diffusion reshapes it) and FIXED cells (exported). The flux carries its
// composition, mixed by volume down the stack.
struct FluvialSettings {
    double kSediment = 0.0; // K on sediment
    double m = 0.5;
    double g = 0.0;         // deposition coefficient G
    double dt = 0.0;
    double seaLevel = 0.0;
    double cellM = 0.0;
    double tolerance = 1e-3;
    int maxIterations = 20;
};

struct FluvialResult {
    int iterations = 0;
    double exported = 0.0;  // m3 sent to FIXED cells
    double toSea = 0.0;     // m3 deposited in ocean outlets
    double toLakes = 0.0;   // m3 deposited in lakes
};

// `frozen`: cells under thick ice (no fluvial erosion nor deposit; the flux
// crosses them). `h` (= b + sed after uplift) is updated, as are b, sed and frac through
// `regolith`. `rockK` is k_base * erodibility of the surface rock of each
// cell, `area` the drainage area. Outputs per cell: the flux leaving it
// (m3/yr), the erosion and deposition rates (m/yr) of this step.
FluvialResult fluvialStep(const FlowRouter& router, const FluvialSettings& settings, const Regolith& regolith,
                          const std::vector<double>& discharge, const std::vector<double>& area,
                          const std::vector<double>& rockK, const std::vector<std::uint8_t>& fixed,
                          std::vector<double>& h, std::vector<double>& sedimentFlux, std::vector<double>& erosionRate,
                          std::vector<double>& depositionRate, const std::vector<std::uint8_t>* frozen = nullptr);

} // namespace wg
