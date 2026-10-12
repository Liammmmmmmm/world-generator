#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <FastNoiseLite.h>

namespace wg {

// Ridged multifractal noise (Musgrave), in [0, 1]. Each octave is
// (1 - |noise|)², sharp crests along the zero lines of the noise, weighted by
// the previous octave's value: the detail concentrates on the ridges and the
// valleys stay smooth.
class Ridged {
public:
    struct Settings {
        double frequency = 1.0; // cycles per unit of the input coordinates
        int octaves = 1;
        double lacunarity = 2.0;
        double gain = 0.5; // amplitude ratio between octaves
    };

    Ridged(std::uint64_t seed, const Settings& settings) : settings_(settings) {
        for (int o = 0; o < settings.octaves; ++o) { // one decorrelated noise per octave
            FastNoiseLite& noise = octaves_.emplace_back();
            noise.SetSeed(static_cast<int>((seed ^ (seed >> 32)) + 7919u * static_cast<std::uint64_t>(o)));
            noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
            noise.SetFrequency(1.0f);
        }
    }

    float operator()(double x, double y) const {
        double sum = 0.0, norm = 0.0, amplitude = 1.0, weight = 1.0, frequency = settings_.frequency;
        for (const FastNoiseLite& noise : octaves_) {
            double signal = 1.0 - std::abs(noise.GetNoise(x * frequency, y * frequency));
            signal *= signal * weight;
            weight = std::clamp(2.0 * signal, 0.0, 1.0); // Musgrave's weighting gain of 2
            sum += signal * amplitude;
            norm += amplitude;
            amplitude *= settings_.gain;
            frequency *= settings_.lacunarity;
        }
        return static_cast<float>(norm > 0.0 ? sum / norm : 0.0);
    }

private:
    Settings settings_;
    std::vector<FastNoiseLite> octaves_;
};

} // namespace wg
