#pragma once

#include <array>
#include <cstdint>

namespace wg {

// 2D gradient (Perlin) noise with a seeded permutation table.
class Perlin2D {
public:
    static constexpr int kMaxOctaves = 16;

    explicit Perlin2D(std::uint64_t seed);

    // Single octave, roughly in [-1, 1], period 256.
    double noise(double x, double y) const;

    // Fractal sum of octaves, normalized to roughly [-1, 1]. Each octave is
    // offset so their lattices do not line up.
    double fbm(double x, double y, int octaves, double lacunarity, double persistence) const;

private:
    std::array<std::uint8_t, 512> perm_{};
    std::array<double, kMaxOctaves * 2> octaveOffsets_{};
};

} // namespace wg
