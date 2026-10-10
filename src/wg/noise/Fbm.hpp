#pragma once

#include <cstdint>

#include <FastNoiseLite.h>

namespace wg {

// Fractal (fBm) OpenSimplex2 noise, roughly in [-1, 1].
class Fbm {
public:
    struct Settings {
        double frequency = 1.0; // cycles per unit of the input coordinates
        int octaves = 1;
        double lacunarity = 2.0;
        double gain = 0.5;
    };

    Fbm(std::uint64_t seed, const Settings& settings) {
        noise_.SetSeed(static_cast<int>(seed ^ (seed >> 32)));
        noise_.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        noise_.SetFrequency(static_cast<float>(settings.frequency));
        noise_.SetFractalType(settings.octaves > 1 ? FastNoiseLite::FractalType_FBm : FastNoiseLite::FractalType_None);
        noise_.SetFractalOctaves(settings.octaves);
        noise_.SetFractalLacunarity(static_cast<float>(settings.lacunarity));
        noise_.SetFractalGain(static_cast<float>(settings.gain));
    }

    float operator()(double x, double y) const { return noise_.GetNoise(x, y); }

private:
    FastNoiseLite noise_;
};

} // namespace wg
