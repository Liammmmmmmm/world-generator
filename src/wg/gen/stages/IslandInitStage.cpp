#include "wg/gen/stages/IslandInitStage.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "wg/core/Random.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/island/Geology.hpp"
#include "wg/island/IslandGrid.hpp"
#include "wg/island/IslandLayers.hpp"
#include "wg/island/Relief.hpp"
#include "wg/noise/Fbm.hpp"

namespace wg {

namespace {

// Size of the random perturbation added to every height (m), so that flat
// areas (a flat initial topography, the shelf) are never exactly flat: water
// on them still has a direction to follow. 1 mm is far below anything visible.
constexpr double kHeightJitterM = 1e-3;

double smoothstep(double edge0, double edge1, double x) {
    if (edge0 == edge1)
        return x < edge0 ? 0.0 : 1.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

// Gaussian bump, 1 at 0, with unit standard deviation.
double gauss(double t) {
    return std::exp(-0.5 * t * t);
}

// Polynomial smooth minimum: min(a, b), rounded over a band of width k.
double smoothMin(double a, double b, double k) {
    if (k <= 0.0)
        return std::min(a, b);
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k / 4.0;
}

double smoothMax(double a, double b, double k) {
    return -smoothMin(-a, -b, k);
}

// Elliptical lobe: center, semi-axes (a along its orientation, b across).
struct Lobe {
    double x, y;
    double a, b;
    double cosAngle, sinAngle;

    // Signed distance to the ellipse, > 0 inside. First-order approximation
    // f / |grad f| of the implicit function f = |(u/a, v/b)| - 1: exact along
    // both axes near the outline, close enough elsewhere for a coastline.
    // Inside, it is capped at the semi-minor axis (the largest distance to the
    // outline that exists in an ellipse): uncapped, it tends to a along the
    // major axis but to b across it, a discontinuity at the center.
    double signedDistance(double px, double py) const {
        const double dx = px - x, dy = py - y;
        const double u = dx * cosAngle + dy * sinAngle, v = -dx * sinAngle + dy * cosAngle;
        const double k = std::hypot(u / a, v / b);
        const double inradius = std::min(a, b);
        if (k < 1e-9)
            return inradius;
        const double gradient = std::hypot(u / (a * a), v / (b * b)) / k;
        return std::min((1.0 - k) / gradient, inradius);
    }

    // Point of the outline at parametric angle t, and the outward normal there.
    void outlinePoint(double t, double& px, double& py, double& nx, double& ny) const {
        const double u = a * std::cos(t), v = b * std::sin(t);
        px = x + u * cosAngle - v * sinAngle;
        py = y + u * sinAngle + v * cosAngle;
        const double gu = std::cos(t) / a, gv = std::sin(t) / b, norm = std::hypot(gu, gv);
        nx = (gu * cosAngle - gv * sinAngle) / norm;
        ny = (gu * sinAngle + gv * cosAngle) / norm;
    }
};

struct Bay {
    double x, y, radius;
};

// --- Volcanic relief --------------------------------------------------------------

using Volcano = IslandParams::InitialTopography::Volcano;

struct Vent {
    double x, y;       // meters from the map center
    double scale = 1.0; // the volcano shrunk to fit the island around its vent
};

// Height of a volcano at distance r from its vent: power-law cone, with either
// a caldera (summit cut at the profile height at the caldera radius, flat floor
// caldera_depth_m lower, smoothstep wall) or a crater (parabolic hollow).
double coneHeight(const Volcano& v, double r) {
    const auto profile = [&](double d) {
        return v.heightM * std::pow(std::max(0.0, 1.0 - d / v.radiusM), v.profileExponent);
    };
    if (v.caldera) {
        const double rim = profile(v.calderaRadiusM);
        if (r >= v.calderaRadiusM)
            return profile(r);
        return lerp(rim - v.calderaDepthM, rim, smoothstep(v.calderaRadiusM - v.calderaRimWidthM, v.calderaRadiusM, r));
    }
    const double hollow = std::max(0.0, 1.0 - r / v.craterRadiusM);
    return profile(r) - v.craterDepthM * hollow * hollow;
}

// Vents placed by rejection in the inner half of the island (distance to the
// coast at least half its maximum), at least min_spacing_m apart; fewer vents
// if the island has no room. Each volcano is then scaled down (as a whole:
// radius, height, crater, caldera) so that its foot reaches the nearest coast,
// instead of being cut by it into half a cone and a cliff.
std::vector<Vent> placeVents(const Volcano& v, const Grid2D<float>& coastDist, double coastFalloffM,
                             const IslandGrid& grid, Rng& rng) {
    constexpr int kAttemptsPerVent = 1000;
    const auto maxDist = std::max_element(coastDist.data().begin(), coastDist.data().end());
    std::vector<Vent> vents;
    for (int attempt = 0; attempt < kAttemptsPerVent * v.count && static_cast<int>(vents.size()) < v.count; ++attempt) {
        const int i = rng.nextInt(0, grid.nx - 1), j = rng.nextInt(0, grid.ny - 1);
        const double toCoast = coastDist.at(i, j);
        if (toCoast < 0.5 * *maxDist || toCoast <= 0.0)
            continue;
        const Vent candidate{grid.x(i), grid.y(j), std::min(1.0, (toCoast + coastFalloffM / 2.0) / v.radiusM)};
        const bool spaced = std::all_of(vents.begin(), vents.end(), [&](const Vent& other) {
            return std::hypot(candidate.x - other.x, candidate.y - other.y) >= v.minSpacingM;
        });
        if (spaced)
            vents.push_back(candidate);
    }
    return vents;
}

// Lava flows: random walks from just outside each vent's crater (or caldera)
// down the steepest slope, the direction deviating randomly at each step. Each
// leaves a Gaussian ridge along its path; it stops at sea or on flat ground.
void addLavaFlows(Grid2D<float>& land, const Grid2D<float>& mask, const IslandGrid& grid, const Volcano& v,
                  const std::vector<Vent>& vents, Rng& rng) {
    Grid2D<float> flows(grid.nx, grid.ny);
    const double sigma = v.lavaFlowWidthM / 2.0;
    const int reach = static_cast<int>(std::ceil(3.0 * sigma / grid.cellM));
    const double minSlope = std::tan(v.lavaFlowMinSlopeDeg * std::numbers::pi / 180.0);
    const double turn = v.lavaFlowTurnDeg * std::numbers::pi / 180.0;
    const double summit = 1.1 * (v.caldera ? v.calderaRadiusM : v.craterRadiusM); // just outside the summit hollow
    const auto cellOf = [&](double x, double y, int& i, int& j) {
        i = static_cast<int>(std::floor(x / grid.cellM + grid.nx * 0.5));
        j = static_cast<int>(std::floor(y / grid.cellM + grid.ny * 0.5));
        return i >= 0 && j >= 0 && i < grid.nx && j < grid.ny;
    };

    for (const Vent& vent : vents) {
        for (int f = 0; f < v.lavaFlowCount; ++f) {
            const double angle = 2.0 * std::numbers::pi * rng.nextDouble();
            const double start = summit * vent.scale;
            double x = vent.x + start * std::cos(angle), y = vent.y + start * std::sin(angle);
            for (int step = 0; step < grid.nx + grid.ny; ++step) {
                int i, j;
                if (!cellOf(x, y, i, j) || mask.at(i, j) < 0.5f)
                    break;
                const double gx = (land.atClamped(i + 1, j) - land.atClamped(i - 1, j)) / (2.0 * grid.cellM);
                const double gy = (land.atClamped(i, j + 1) - land.atClamped(i, j - 1)) / (2.0 * grid.cellM);
                if (std::hypot(gx, gy) < minSlope)
                    break;
                for (int dj = -reach; dj <= reach; ++dj) {
                    for (int di = -reach; di <= reach; ++di) {
                        if (!flows.contains(i + di, j + dj))
                            continue;
                        const double d = std::hypot(grid.x(i + di) - x, grid.y(j + dj) - y);
                        float& cell = flows.at(i + di, j + dj);
                        cell = std::max(cell, static_cast<float>(v.lavaFlowHeightM * gauss(d / sigma)));
                    }
                }
                const double direction = std::atan2(-gy, -gx) + turn * (2.0 * rng.nextDouble() - 1.0);
                x += grid.cellM * std::cos(direction);
                y += grid.cellM * std::sin(direction);
            }
        }
    }
    for (std::size_t k = 0; k < land.size(); ++k)
        land.data()[k] += flows.data()[k];
}

// Box blur of radius `radius` cells, twice (close to a Gaussian), separable,
// with clamped borders.
Grid2D<float> blurred(const Grid2D<float>& grid, int radius) {
    Grid2D<float> a = grid, b(grid.width(), grid.length());
    if (radius <= 0)
        return a;
    const double norm = 1.0 / (2 * radius + 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = 0; j < a.length(); ++j) { // horizontal: a -> b
            double sum = 0.0;
            for (int k = -radius; k <= radius; ++k)
                sum += a.atClamped(k, j);
            for (int i = 0; i < a.width(); ++i) {
                b.at(i, j) = static_cast<float>(sum * norm);
                sum += a.atClamped(i + radius + 1, j) - a.atClamped(i - radius, j);
            }
        }
        for (int i = 0; i < a.width(); ++i) { // vertical: b -> a
            double sum = 0.0;
            for (int k = -radius; k <= radius; ++k)
                sum += b.atClamped(i, k);
            for (int j = 0; j < a.length(); ++j) {
                a.at(i, j) = static_cast<float>(sum * norm);
                sum += b.atClamped(i, j + radius + 1) - b.atClamped(i, j - radius);
            }
        }
    }
    return a;
}

// Reports progress every few rows of a pass covering [from, to] of the stage.
struct RowProgress {
    const StageContext& context;
    double from, to;
    int rows;

    void operator()(int j) const {
        if (j % 32 == 0)
            context.progress(static_cast<float>(lerp(from, to, static_cast<double>(j) / rows)));
    }
};

} // namespace

void IslandInitStage::run(WorldState& world, const StageContext& context) {
    const IslandParams& p = world.params().values;
    const IslandGrid grid(p);
    const double R = grid.radiusM;
    const double sea = context.info.seaLevelM(); // config depths and elevations are relative to it
    const std::uint64_t seed = context.info.seed;

    // All layers are created up front, in display order.
    Grid2D<float>& h = world.createLayer(layers::kHeight, grid.nx, grid.ny, 0.0f, {LayerStyle::Height, {}});
    Grid2D<float>& mask = world.createLayer(layers::kMask, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& coastDist = world.createLayer(layers::kCoastDist, grid.nx, grid.ny, 0.0f, {LayerStyle::Diverging, {}});
    Grid2D<float>& lagoon = world.createLayer(layers::kLagoon, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& relief = world.createLayer(layers::kRelief, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    const Geology geology(p, &h); // volcanic strata follow the surface, final once built
    Grid2D<float>& foldOffset = world.createLayer(layers::kFoldOffset, grid.nx, grid.ny, 0.0f, {LayerStyle::Diverging, {}});
    Grid2D<float>& surfaceMaterial = world.createLayer(layers::kSurfaceMaterial, grid.nx, grid.ny, 0.0f,
                                                       {LayerStyle::Categorical, geology.materialNames()});
    Grid2D<std::uint8_t>& flags = world.createByteLayer(layers::kFlags, grid.nx, grid.ny);

    // --- 1. Land mask, from a signed distance to the coast -------------------
    //
    // The lobes live in a "shape frame" aligned with the main axis and
    // compressed along it by the elongation: (U, V) = (u / elongation, v), so a
    // round lobe there is an island stretched along the axis in the world.
    const auto& shape = p.shape;
    const std::uint64_t shapeSeed = deriveSeed(seed, "shape");
    const double axis = shape.mainAxisDeg * std::numbers::pi / 180.0;
    const double cosAxis = std::cos(axis), sinAxis = std::sin(axis);
    const double elongation = shape.elongation;

    std::vector<Lobe> lobes;
    Rng lobeRng(deriveSeed(shapeSeed, "lobes"));
    for (int i = 0; i < shape.lobeCount; ++i) {
        // Always 5 draws per lobe, so changing one range does not reshuffle the others.
        const double angle = 2.0 * std::numbers::pi * lobeRng.nextDouble();
        const double distance = std::sqrt(lobeRng.nextDouble()) * R * shape.lobeSpread;
        const double radius = R * lerp(shape.lobeRadiusMin, shape.lobeRadiusMax, lobeRng.nextDouble());
        const double aspect = lerp(1.0, shape.lobeElongationMax, lobeRng.nextDouble());
        const double turn = (lobeRng.nextDouble() - 0.5) * std::numbers::pi; // random orientation offset
        // Center uniform in a disk of the shape frame (so spread mostly along the
        // axis in the world); orientation along the axis, plus an offset that
        // the alignment removes.
        const double orientation = (1.0 - shape.lobeAlignment) * turn;
        lobes.push_back({distance * std::cos(angle), distance * std::sin(angle), radius * std::sqrt(aspect),
                         radius / std::sqrt(aspect), std::cos(orientation), std::sin(orientation)});
    }
    // Scale the lobes so that the island fits in R (half its length along the
    // axis): spread centers and large lobes could otherwise reach 2R or more.
    double extent = 0.0;
    for (const Lobe& lobe : lobes)
        extent = std::max(extent, std::hypot(lobe.x, lobe.y) + std::max(lobe.a, lobe.b));
    const double scale = R / (extent * elongation);
    for (Lobe& lobe : lobes) {
        lobe.x *= scale;
        lobe.y *= scale;
        lobe.a *= scale;
        lobe.b *= scale;
    }
    // From the shape frame to the world, for points and for normals.
    const auto toWorld = [&](double U, double V, double& x, double& y) {
        const double u = U * elongation;
        x = u * cosAxis - V * sinAxis;
        y = u * sinAxis + V * cosAxis;
    };

    // Bays (subtractive lobes): disks placed on the outline of a random lobe,
    // pushed inland by bay_penetration. Defined in the world frame.
    std::vector<Bay> bays;
    Rng bayRng(deriveSeed(shapeSeed, "bays"));
    for (int i = 0; i < shape.bayCount; ++i) {
        const Lobe& lobe = lobes[std::min(lobes.size() - 1, static_cast<std::size_t>(bayRng.nextDouble() * lobes.size()))];
        const double t = 2.0 * std::numbers::pi * bayRng.nextDouble();
        const double radius = R * lerp(shape.bayRadiusMin, shape.bayRadiusMax, bayRng.nextDouble());
        double pu, pv, nu, nv, px, py, nx, ny;
        lobe.outlinePoint(t, pu, pv, nu, nv);
        toWorld(pu, pv, px, py);
        toWorld(nu / (elongation * elongation), nv, nx, ny); // normals transform with the inverse scale
        const double norm = std::hypot(nx, ny);
        const double inland = radius * (2.0 * shape.bayPenetration - 1.0); // < 0: center offshore
        bays.push_back({px - nx / norm * inland, py - ny / norm * inland, radius});
    }
    const double blend = shape.lobeBlend * R / elongation;

    // The map edge must always be preceded by the sea side of the coast
    // transition, the full shelf and the full slope, so the sea floor reaches
    // the boundary depth exactly at the FIXED ring. With the default parameters
    // there is room to spare (see freeSeaWidthM in IslandParams.cpp); this only
    // cuts into the island when the config forces a too large shelf or slope.
    const double seaRing = shape.coastFalloffM / 2.0 + shape.shelfWidthM + shape.talusWidthM;
    const double halfX = grid.nx * grid.cellM / 2.0, halfY = grid.ny * grid.cellM / 2.0;

    // Domain warp of the position (not of the value), by two decorrelated noises,
    // scaled to the island half-width R / elongation.
    const double warp = R * shape.noise.amplitude / elongation;
    const Fbm::Settings warpSettings{shape.noise.frequency, shape.noise.octaves, shape.noise.lacunarity,
                                     shape.noise.gain};
    const Fbm warpX(deriveSeed(shapeSeed, "warp_x"), warpSettings);
    const Fbm warpY(deriveSeed(shapeSeed, "warp_y"), warpSettings);

    // Pass 1: distance to the lobes in the shape frame, at the warped position.
    Grid2D<float> lobeDist(grid.nx, grid.ny), warpedX(grid.nx, grid.ny), warpedY(grid.nx, grid.ny);
    const RowProgress lobeProgress{context, 0.0, 0.3, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        lobeProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            const double qx = grid.x(i) / R, qy = grid.y(j) / R;
            const double px = grid.x(i) + warp * warpX(qx, qy);
            const double py = grid.y(j) + warp * warpY(qx, qy);
            const double U = (px * cosAxis + py * sinAxis) / elongation, V = -px * sinAxis + py * cosAxis;
            double s = lobes.front().signedDistance(U, V);
            for (std::size_t k = 1; k < lobes.size(); ++k)
                s = smoothMax(s, lobes[k].signedDistance(U, V), blend);
            lobeDist.at(i, j) = static_cast<float>(s);
            warpedX.at(i, j) = static_cast<float>(px);
            warpedY.at(i, j) = static_cast<float>(py);
        }
    }

    // Pass 2: back to meters. A distance measured in the shape frame is
    // stretched along the axis; dividing by its gradient norm in the world
    // (f / |grad f|) restores a distance in meters, which the shelf and slope
    // widths rely on.
    // Distance to the outline of the shape (the "disk"), stored in coastDist.
    const RowProgress diskProgress{context, 0.3, 0.35, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        diskProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, grid.nx - 1);
            const int j0 = std::max(j - 1, 0), j1 = std::min(j + 1, grid.ny - 1);
            const double gx = (lobeDist.at(i1, j) - lobeDist.at(i0, j)) / ((i1 - i0) * grid.cellM);
            const double gy = (lobeDist.at(i, j1) - lobeDist.at(i, j0)) / ((j1 - j0) * grid.cellM);
            // A distance stretched by the elongation has a gradient norm within
            // [1 / elongation, 1]; the warp makes the measured one noisy, and
            // outside that range it would only add that noise to the distance
            // (and to the heights through the coast ramp).
            double s = lobeDist.at(i, j) / std::clamp(std::hypot(gx, gy), 1.0 / elongation, 1.0);

            const double px = warpedX.at(i, j), py = warpedY.at(i, j);
            for (const Bay& bay : bays) // carve: outside the island inside a bay
                s = smoothMin(s, std::hypot(px - bay.x, py - bay.y) - bay.radius, shape.coastFalloffM);
            coastDist.at(i, j) = static_cast<float>(s);
        }
    }

    // Ring mode (atoll): the land is a reef ring following the outline, set
    // back by ring_offset_m, around a lagoon. Its width varies with a noise and
    // drops below zero in places: the passes, which cut the ring into motus.
    if (shape.mode == "ring") {
        const Fbm::Settings ringSettings{shape.ringNoiseFrequency, shape.ringNoiseOctaves, shape.noise.lacunarity,
                                         shape.noise.gain};
        const Fbm passNoise(deriveSeed(shapeSeed, "ring_passes"), ringSettings);
        const Fbm widthNoise(deriveSeed(shapeSeed, "ring_width"), ringSettings);
        Grid2D<float> passes(grid.nx, grid.ny);
        std::vector<float> centerline; // pass noise along the ring axis, to calibrate the passes
        for (int j = 0; j < grid.ny; ++j) {
            for (int i = 0; i < grid.nx; ++i) {
                passes.at(i, j) = passNoise(grid.x(i) / R, grid.y(j) / R);
                if (std::abs(coastDist.at(i, j) - shape.ringOffsetM) < grid.cellM)
                    centerline.push_back(passes.at(i, j));
            }
        }
        // Width = ring_width_m * (n - q) / (mean - q) * (1 + ring_width_noise * m):
        //  - n is the pass noise and q its ring_pass_fraction quantile along the
        //    ring axis, so exactly that share of the ring has a negative width
        //    (the passes), and the mean width is ring_width_m;
        //  - m is an independent noise that only varies the width (the factor
        //    stays positive, so it does not move the passes).
        double q = -2.0, mean = 0.0; // fbm is within [-1, 1]: no pass by default
        if (!centerline.empty()) {
            std::sort(centerline.begin(), centerline.end());
            if (shape.ringPassFraction > 0.0)
                q = centerline[std::min(centerline.size() - 1,
                                        static_cast<std::size_t>(shape.ringPassFraction * centerline.size()))];
            for (float n : centerline)
                mean += n;
            mean /= centerline.size();
        }
        const double scale = shape.ringWidthM / std::max(mean - q, 1e-6);
        for (int j = 0; j < grid.ny; ++j) {
            for (int i = 0; i < grid.nx; ++i) {
                const double inward = coastDist.at(i, j) - shape.ringOffsetM; // > 0: inside the ring axis
                const double variation = 1.0 + shape.ringWidthNoise * widthNoise(grid.x(i) / R, grid.y(j) / R);
                const double width = scale * (passes.at(i, j) - q) * std::max(variation, 0.0);
                const double s = width / 2.0 - std::abs(inward);
                coastDist.at(i, j) = static_cast<float>(s);
                if (inward > 0.0 && s < 0.0) {
                    lagoon.at(i, j) = 1.0f;
                    flags.at(i, j) |= layers::kInLagoon;
                }
            }
        }
    }

    // Map edge constraint, then the mask.
    const RowProgress maskProgress{context, 0.35, 0.4, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        maskProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            const double edgeDistance = std::min(halfX - std::abs(grid.x(i)), halfY - std::abs(grid.y(j)));
            const double s = smoothMin(coastDist.at(i, j), edgeDistance - grid.cellM / 2.0 - seaRing, shape.coastFalloffM);
            coastDist.at(i, j) = static_cast<float>(s);
            mask.at(i, j) = static_cast<float>(smoothstep(-shape.coastFalloffM / 2, shape.coastFalloffM / 2, s));
        }
    }

    // --- 2. Land relief (relative to sea level), by topography mode ---------
    // The domain warp's fine octaves, which make a jagged coastline, also make
    // coast_dist irregular far inland and offshore. Heights follow the
    // distance to the coast over wide bands (coast ramps, shelf and slope,
    // barrier crest), so they use a smoothed
    // distance (blurred over a quarter of the coast transition): a blur keeps a
    // linear field, so it is still the distance away from the coast and ~0 on
    // it, without the noise. The coastline itself stays the raw distance.
    const Grid2D<float> smoothDist =
        blurred(coastDist, static_cast<int>(std::lround(shape.coastFalloffM / 4.0 / grid.cellM)));

    const auto& topo = p.initialTopography;
    const std::uint64_t topoSeed = deriveSeed(seed, "topo");
    const Fbm topoNoise(topoSeed, {topo.noise.frequency, topo.noise.octaves, topo.noise.lacunarity, topo.noise.gain});
    Grid2D<float> land(grid.nx, grid.ny);

    // Barrier crest: highest where the island is widest from both coasts.
    double halfWidth = 0.0;
    if (topo.mode == "barrier")
        for (float d : smoothDist.data())
            halfWidth = std::max(halfWidth, static_cast<double>(d));

    Rng volcanoRng(deriveSeed(topoSeed, "volcanoes"));
    const std::vector<Vent> vents =
        topo.mode == "volcanic_cone" ? placeVents(topo.volcano, coastDist, shape.coastFalloffM, grid, volcanoRng)
                                     : std::vector<Vent>{};

    const Fbm flankNoise(deriveSeed(topoSeed, "flanks"), {topo.volcano.flankNoiseFrequency, topo.volcano.flankNoiseOctaves,
                                                           topo.noise.lacunarity, topo.noise.gain});

    const RowProgress landProgress{context, 0.4, 0.5, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        landProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            const double n = topoNoise(grid.x(i) / R, grid.y(j) / R);
            double hLand = topo.baseElevationM;
            if (topo.mode == "peneplain" || topo.mode == "noise") {
                hLand += topo.noise.amplitudeM * n;
            } else if (topo.mode == "barrier") {
                hLand += topo.barrier.crestHeightM * smoothstep(0.0, halfWidth, smoothDist.at(i, j)) +
                         topo.noise.amplitudeM * n;
            } else if (topo.mode == "atoll") {
                hLand = topo.atoll.ringHeightM + topo.atoll.ringNoiseM * n;
            } else if (topo.mode == "volcanic_cone") {
                // Cones (scaled volcanoes: geometric similarity) rising from the base
                // plateau, the junction rounded over the plateau height.
                for (const Vent& vent : vents) {
                    const double r = std::hypot(grid.x(i) - vent.x, grid.y(j) - vent.y);
                    hLand = smoothMax(hLand, vent.scale * coneHeight(topo.volcano, r / vent.scale), topo.baseElevationM);
                }
                // Irregular flanks: on a perfect cone the steepest descent is radial
                // everywhere and every lava flow would be a straight spoke.
                hLand += topo.volcano.flankNoiseM * flankNoise(grid.x(i) / R, grid.y(j) / R);
            }
            land.at(i, j) = static_cast<float>(hLand);
        }
    }
    // Mountains of the relief zones, on top of the initial topography (and
    // under the lava flows, which follow them).
    relief = reliefHeights(p, grid, mask, seed, [&](double t) { context.progress(static_cast<float>(lerp(0.5, 0.6, t))); });
    for (std::size_t k = 0; k < land.size(); ++k)
        land.data()[k] += relief.data()[k];

    if (topo.mode == "volcanic_cone")
        addLavaFlows(land, mask, grid, topo.volcano, vents, volcanoRng);

    // Atoll lagoon floor: noisy, with a few pinnacles rising close to the surface.
    const auto& atoll = topo.atoll;
    const bool atollLagoon = topo.mode == "atoll";
    const Fbm lagoonNoise(deriveSeed(topoSeed, "lagoon"),
                          {topo.noise.frequency, topo.noise.octaves, topo.noise.lacunarity, topo.noise.gain});
    std::vector<Vent> pinnacles;
    if (atollLagoon) {
        Rng pinnacleRng(deriveSeed(topoSeed, "pinnacles"));
        for (int attempt = 0; attempt < 1000 * atoll.pinnacleCount &&
                              static_cast<int>(pinnacles.size()) < atoll.pinnacleCount;
             ++attempt) {
            const int i = pinnacleRng.nextInt(0, grid.nx - 1), j = pinnacleRng.nextInt(0, grid.ny - 1);
            if (lagoon.at(i, j) > 0.5f)
                pinnacles.push_back({grid.x(i), grid.y(j)});
        }
    }
    const auto lagoonFloor = [&](int i, int j) {
        double floor = -atoll.lagoonDepthM + atoll.lagoonNoiseM * lagoonNoise(grid.x(i) / R, grid.y(j) / R);
        for (const Vent& pinnacle : pinnacles) {
            const double d = std::hypot(grid.x(i) - pinnacle.x, grid.y(j) - pinnacle.y);
            floor = std::max(floor, lerp(floor, -atoll.pinnacleTopDepthM, gauss(d / atoll.pinnacleRadiusM)));
        }
        return std::min(floor, -atoll.pinnacleTopDepthM);
    };

    // --- 3. Bathymetry and combination ----------------------------------------

    // Low-frequency relief of the deep sea floor: a single octave.
    const Fbm seafloorNoise(deriveSeed(seed, "seafloor"), {shape.seafloorNoise.frequency, 1, 2.0, 0.5});
    Rng jitter(deriveSeed(topoSeed, "jitter"));
    const double boundaryDepth = p.simulation.boundaryDepthM;

    const RowProgress heightProgress{context, 0.6, 0.75, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        heightProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            const double qx = grid.x(i) / R, qy = grid.y(j) / R;
            const bool inLagoon = lagoon.at(i, j) > 0.5f;

            // Open sea: flat shelf, then a slope down to the boundary depth. Sea
            // floor noise is strongest mid-slope and zero at both ends (so the floor
            // meets the FIXED ring without a step), never above the shelf. A lagoon
            // has its own floor (atoll topography) or stays at the shelf depth.
            double hSea = shape.shelfDepthM;
            const double offshore = -smoothDist.at(i, j);
            if (inLagoon) {
                if (atollLagoon)
                    hSea = lagoonFloor(i, j);
            } else if (offshore > shape.shelfWidthM) {
                const double slope = smoothstep(0.0, shape.talusWidthM, offshore - shape.shelfWidthM);
                const double noiseWeight = 4.0 * slope * (1.0 - slope);
                hSea = std::min(shape.shelfDepthM, lerp(shape.shelfDepthM, boundaryDepth, slope) +
                                                       noiseWeight * shape.seafloorNoise.amplitudeM *
                                                           seafloorNoise(qx, qy));
            }

            // Each side joins sea level exactly on the coast (s = 0, mask = 0.5) over
            // half the coast transition. A plain lerp(hSea, hLand, mask) would put
            // the coastline wherever the blend crosses 0, far into the transition
            // band when hLand >> |hSea|, where the warp's fine octaves turn it into
            // speckles. Land below sea level is kept at sea level.
            // (side from the raw distance, ramp from the smoothed one, on its side of 0)
            const bool onLand = coastDist.at(i, j) >= 0.0f;
            const double d = onLand ? std::max(0.0f, smoothDist.at(i, j)) : std::max(0.0f, -smoothDist.at(i, j));
            const double ramp = smoothstep(0.0, shape.coastFalloffM / 2.0, d);
            double height = sea + (onLand ? std::max(static_cast<double>(land.at(i, j)), 0.0) : hSea) * ramp;
            height += kHeightJitterM * jitter.nextDouble(); // see kHeightJitterM
            h.at(i, j) = static_cast<float>(height);
        }
    }

    // --- 4. Boundary conditions ----------------------------------------------
    for (int j = 0; j < grid.ny; ++j) {
        for (int i = 0; i < grid.nx; ++i) {
            if (grid.isBorder(i, j)) {
                flags.at(i, j) |= layers::kFixed;
                h.at(i, j) = static_cast<float>(sea + boundaryDepth);
            }
        }
    }

    // --- 5. Surface material -------------------------------------------------
    const RowProgress materialProgress{context, 0.75, 0.95, grid.ny};
    for (int j = 0; j < grid.ny; ++j) {
        materialProgress(j);
        for (int i = 0; i < grid.nx; ++i) {
            foldOffset.at(i, j) = static_cast<float>(geology.foldOffset(grid.x(i), grid.y(j)));
            surfaceMaterial.at(i, j) = static_cast<float>(geology.material(grid.x(i), grid.y(j), h.at(i, j)));
        }
    }

    for (float value : h.data())
        if (!std::isfinite(value))
            throw WorldError("non-finite altitude produced by the island initialization");

    // Reference surface for volcanic strata, and the start of the erosion.
    world.createLayer(layers::kHeightInitial, grid.nx, grid.ny, 0.0f, {LayerStyle::Height, {}}) = h;
    context.progress(1.0f);
}

} // namespace wg
