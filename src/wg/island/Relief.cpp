#include "wg/island/Relief.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

#include "wg/core/Random.hpp"
#include "wg/noise/Fbm.hpp"
#include "wg/noise/Ridged.hpp"

namespace wg {

namespace {

// Low-frequency noises of the zones (cycles per island radius).
constexpr double kZoneWarpFrequency = 1.5;
constexpr double kModulationFrequency = 1.0;
// Fraction of a range's half-length over which it fades at both ends.
constexpr double kRangeEndFalloff = 0.6;
// A hills zone is flat-topped: full amplitude up to this fraction of its
// half-size, then fades to zero at its edge.
constexpr double kHillsPlateau = 0.3;
// Share of a zone's height that does not depend on the ridged noise: the
// ridges stand on a mountain mass instead of rising from a plain.
constexpr double kRidgeBody = 0.35;
// The noise warp is smoother than the noise itself (fraction of its frequency).
constexpr double kNoiseWarpFrequency = 0.5;

double smoothstep(double edge0, double edge1, double x) {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double gauss(double t) {
    return std::exp(-0.5 * t * t);
}

struct Zone {
    const IslandParams::ReliefZone* params;
    bool range;
    bool hills;
    double cosAngle, sinAngle;
    double halfLength, halfWidth;
};

// Amplitude pattern of a zone at (u, v), its own frame (u along its axis).
double amplitude(const Zone& z, double u, double v) {
    if (z.range) // band across the axis, fading at both ends
        return gauss(v / z.halfWidth) * (1.0 - smoothstep(1.0 - kRangeEndFalloff, 1.0, std::abs(u) / z.halfLength));
    const double rho = std::hypot(u / z.halfLength, v / z.halfWidth);
    return z.hills ? 1.0 - smoothstep(kHillsPlateau, 1.0, rho) : gauss(rho);
}

} // namespace

Grid2D<float> reliefHeights(const IslandParams& params, const IslandGrid& grid, const Grid2D<float>& mask,
                            std::uint64_t seed, const std::function<void(double)>& progress) {
    Grid2D<float> out(grid.nx, grid.ny);
    const auto& relief = params.relief;
    if (relief.zones.empty())
        return out;

    const double R = grid.radiusM;
    std::vector<Zone> zones;
    for (const auto& z : relief.zones) {
        const double a = z.angleDeg * std::numbers::pi / 180.0;
        zones.push_back({&z, z.type == "range", z.type == "hills", std::cos(a), std::sin(a), z.lengthM / 2.0,
                         z.widthM / 2.0});
    }

    const std::uint64_t reliefSeed = deriveSeed(seed, "relief");
    const Fbm zoneWarpX(deriveSeed(reliefSeed, "zone_warp_x"), {kZoneWarpFrequency, 3, 2.0, 0.5});
    const Fbm zoneWarpY(deriveSeed(reliefSeed, "zone_warp_y"), {kZoneWarpFrequency, 3, 2.0, 0.5});
    const Fbm modulation(deriveSeed(reliefSeed, "modulation"), {kModulationFrequency, 2, 2.0, 0.5});

    const auto& n = relief.mountainNoise;
    const Ridged ridged(deriveSeed(reliefSeed, "mountains"), {n.frequency, n.octaves, n.lacunarity, n.gain});
    // The noise warp moves positions by a fraction of the noise wavelength.
    const Fbm noiseWarpX(deriveSeed(reliefSeed, "noise_warp_x"), {kNoiseWarpFrequency * n.frequency, 2, 2.0, 0.5});
    const Fbm noiseWarpY(deriveSeed(reliefSeed, "noise_warp_y"), {kNoiseWarpFrequency * n.frequency, 2, 2.0, 0.5});
    const double noiseWarp = n.warpAmplitude / n.frequency; // in units of R

    for (int j = 0; j < grid.ny; ++j) {
        if (j % 32 == 0)
            progress(static_cast<double>(j) / grid.ny);
        for (int i = 0; i < grid.nx; ++i) {
            const double fade = smoothstep(0.0, 1.0, mask.at(i, j));
            if (fade <= 0.0)
                continue; // no mountains at sea
            const double qx = grid.x(i) / R, qy = grid.y(j) / R;
            // Zone patterns at a warped position, so that their edges are never straight.
            const double px = grid.x(i) + relief.warpAmplitude * R * zoneWarpX(qx, qy);
            const double py = grid.y(j) + relief.warpAmplitude * R * zoneWarpY(qx, qy);
            const double strength = std::max(0.0, 1.0 + relief.modulation * modulation(qx, qy));
            // Noise coordinates (units of R), warped too, against regular crests.
            const double nx = qx + noiseWarp * noiseWarpX(qx, qy), ny = qy + noiseWarp * noiseWarpY(qx, qy);
            std::optional<double> isotropic; // the same for every zone that is not a range

            double h = 0.0;
            for (const Zone& z : zones) {
                const double dx = px - z.params->center.x, dy = py - z.params->center.y;
                const double u = dx * z.cosAngle + dy * z.sinAngle, v = -dx * z.sinAngle + dy * z.cosAngle;
                const double a = amplitude(z, u, v);
                if (a < 1e-4)
                    continue;
                double noise;
                if (z.range) { // stretched along the range: crests follow its axis
                    const double nu = nx * z.cosAngle + ny * z.sinAngle, nv = -nx * z.sinAngle + ny * z.cosAngle;
                    noise = ridged(nu / n.anisotropy, nv);
                } else {
                    if (!isotropic)
                        isotropic = ridged(nx, ny);
                    noise = *isotropic;
                }
                h += a * z.params->heightM * (kRidgeBody + (1.0 - kRidgeBody) * noise);
            }
            out.at(i, j) = static_cast<float>(h * strength * fade);
        }
    }
    progress(1.0);
    return out;
}

} // namespace wg
