#pragma once

#include "wg/erosion/ErosionMap.hpp"
#include "wg/erosion/Rock.hpp"

namespace wg::erosion {

// Parameters of a drop, in the normalized frame (see ErosionMap).
struct DropParams {
    int maxAge = 512;
    float gravity = 1.0f;
    float momentumTransfer = 1.0f; // pull of the time-averaged flow on the drop
    float entrainment = 10.0f;
    float depositionRate = 0.1f;
    float evaporationRate = 0.001f;
    float minVolume = 0.01f;
    float dischargeScale = 0.4f; // discharge_norm = erf(scale * discharge)
    const Rock* rock = nullptr;  // rock erodibility and talus (required)
    float thermalRate = 0.0f;    // avalanche after each change of a cell; 0: none
};

// Runs one drop of water from (x, y), a position on a land cell, until it dies
// (SimpleHydrology, with the volume-weighted mass transfer of soillib). Gravity
// pulls it down the slope and the time-averaged flow drags it along (momentum
// transfer, which makes rivers meander). Along its path it adds its volume and
// momentum to the tracks of each cell it crosses,
// erodes or deposits on the cell it leaves, and drops what it carries where
// it dies; each modified cell then avalanches. Returns the sediment that left
// the domain (map border).
float descend(ErosionMap& map, const DropParams& params, float x, float y);

} // namespace wg::erosion
