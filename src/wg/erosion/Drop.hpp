#pragma once

#include "wg/erosion/ErosionMap.hpp"

namespace wg::erosion {

// Parameters of a drop, in the normalized frame (see ErosionMap).
struct DropParams {
    int maxAge = 512;
    float gravity = 1.0f;
    float entrainment = 10.0f;
    float depositionRate = 0.1f;
    float evaporationRate = 0.001f;
    float minVolume = 0.01f;
    float dischargeScale = 0.4f; // discharge_norm = erf(scale * discharge)
};

// Runs one drop of water from (x, y), a position on a land cell, until it dies
// (SimpleHydrology, with the volume-weighted mass transfer of soillib). Along
// its path it adds its volume to the discharge track of each cell it crosses,
// erodes or deposits on the cell it leaves, and drops what it carries where
// it dies. Returns the sediment that left the domain (map border).
float descend(ErosionMap& map, const DropParams& params, float x, float y);

} // namespace wg::erosion
