#pragma once

#include <cstdint>

#include "wg/core/Random.hpp"
#include "wg/erosion/Drop.hpp"
#include "wg/erosion/ErosionMap.hpp"

namespace wg::erosion {

struct ParticleErosionParams {
    DropParams drop; // drop.rock must be set
    int particlesPerStep = 0; // 0: land cells / kCellsPerParticle
    float dischargeLrate = 0.1f;
};

// The erosion loop: each step spawns drops on random land cells and runs them
// until they die, then blends what they tracked into the time-averaged maps.
// Deterministic: the drops of step k come from a random stream derived from
// (seed, k), whatever ran before.
class ParticleErosion {
public:
    // Automatic drops per step: one per this many land cells (the density of
    // SimpleHydrology and soillib, 512 drops per step on 512²).
    static constexpr int kCellsPerParticle = 512;

    ParticleErosion(ErosionMap& map, const ParticleErosionParams& params, std::uint64_t seed);

    void step(int index);

    int particlesPerStep() const { return particlesPerStep_; }
    int landCells() const { return landCells_; }

    // Sediment that left the domain so far, and the total mass (all heights
    // plus that sediment), which the erosion conserves.
    double lostSediment() const { return lost_; }
    double totalMass() const;

private:
    // Spawn position: a random land cell, uniformly. Extension point for a
    // precipitation map: accept a candidate with a probability given by it.
    bool spawn(Rng& rng, float& x, float& y) const;

    ErosionMap& map_;
    ParticleErosionParams params_;
    std::uint64_t seed_;
    int landCells_ = 0;
    int particlesPerStep_ = 0;
    double lost_ = 0.0;
};

} // namespace wg::erosion
