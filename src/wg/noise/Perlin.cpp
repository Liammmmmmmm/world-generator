#include "wg/noise/Perlin.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "wg/core/Random.hpp"

namespace wg {

namespace {

double fade(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// Dot product with one of 8 unit gradients.
double grad(std::uint8_t hash, double x, double y) {
    constexpr double d = 0.70710678118654752; // 1/sqrt(2)
    switch (hash & 7) {
    case 0: return x;
    case 1: return -x;
    case 2: return y;
    case 3: return -y;
    case 4: return (x + y) * d;
    case 5: return (x - y) * d;
    case 6: return (-x + y) * d;
    default: return (-x - y) * d;
    }
}

} // namespace

Perlin2D::Perlin2D(std::uint64_t seed) {
    Rng rng(seed);
    std::array<std::uint8_t, 256> p{};
    std::iota(p.begin(), p.end(), std::uint8_t{0});
    for (int i = 255; i > 0; --i) // Fisher-Yates with our portable RNG
        std::swap(p[i], p[rng.nextInt(0, i)]);
    for (int i = 0; i < 512; ++i)
        perm_[i] = p[i & 255];
    for (auto& offset : octaveOffsets_)
        offset = rng.nextDouble() * 256.0;
}

double Perlin2D::noise(double x, double y) const {
    const double fx = std::floor(x), fy = std::floor(y);
    const int X = static_cast<int>(fx) & 255, Y = static_cast<int>(fy) & 255;
    const double xf = x - fx, yf = y - fy;
    const double u = fade(xf), v = fade(yf);

    const int a = perm_[X] + Y, b = perm_[X + 1] + Y;
    const double x1 = lerp(grad(perm_[a], xf, yf), grad(perm_[b], xf - 1.0, yf), u);
    const double x2 = lerp(grad(perm_[a + 1], xf, yf - 1.0), grad(perm_[b + 1], xf - 1.0, yf - 1.0), u);
    return lerp(x1, x2, v) * 1.41421356237309505; // unit gradients peak at 1/sqrt(2)
}

double Perlin2D::fbm(double x, double y, int octaves, double lacunarity, double persistence) const {
    octaves = std::clamp(octaves, 1, kMaxOctaves);
    double sum = 0.0, amplitude = 1.0, frequency = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * noise(x * frequency + octaveOffsets_[o * 2], y * frequency + octaveOffsets_[o * 2 + 1]);
        norm += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    return sum / norm;
}

} // namespace wg
