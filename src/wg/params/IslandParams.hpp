#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wg {

// Fully resolved island generation parameters (see island_params.jsonc for the
// meaning of each field). Every value is set: either drawn from the seed, read
// from the config, or a fixed default.
//
// Units: meters and degrees. Noise frequencies are in cycles per island
// radius (see islandRadiusM), so results do not depend on the grid resolution.
// Map coordinates (x, y) are in meters, with the origin at the map center.
struct IslandParams {
    struct Vec2 {
        double x = 0.0, y = 0.0;
    };

    struct World {
        int width = 0;
        int length = 0;
        double blockSizeM = 1.0;
        int chunkSize = 32;
        int seaLevelY = 0;
        double verticalExaggeration = 1.0;
        int minY = 0; // vertical bounds of the voxel world, in blocks (exclusive max)
        int maxY = 0;
    } world;

    struct Simulation {
        int resolution = 0;         // grid cells across the map width (steps 2 and up)
        double boundaryDepthM = 0.0; // sea floor depth held at the map edge
    } simulation;

    struct Origin {
        std::string type; // volcanic | continental | sedimentary | coral
        std::map<std::string, double> typeWeights;
        double age = 0.0; // 0 = young, 1 = old
    } origin;

    struct Shape {
        std::string mode; // blobs (lobes) | ring (atoll: a reef ring around a lagoon)
        double radiusRatio = 0.0;
        int lobeCount = 0;
        double lobeSpread = 0.0;
        double lobeRadiusMin = 0.0; // lobe (mean) radius range, relative to the island radius
        double lobeRadiusMax = 0.0;
        double lobeElongationMax = 1.0; // each lobe is an ellipse of aspect ratio in [1, max]
        double lobeAlignment = 0.0;     // 0: random lobe orientations, 1: all along the main axis
        double mainAxisDeg = 0.0;       // main axis of the island (also of the first relief range and the folds)
        double elongation = 1.0;        // the whole shape is stretched by this factor along the main axis
        double lobeBlend = 0.0;         // smooth union width between lobes, relative to R
        int bayCount = 0;               // subtractive lobes cut into the coast
        double bayRadiusMin = 0.0;      // bay radius range, relative to R
        double bayRadiusMax = 0.0;
        double bayPenetration = 0.0;    // 0: bays barely touch the coast, 1: they are centered inland
        // ring mode: the reef follows the outline of the blob shape, set back by ringOffsetM
        double ringWidthM = 0.0;
        double ringWidthNoise = 0.0;   // relative variation of the ring width
        double ringNoiseFrequency = 0.0;
        int ringNoiseOctaves = 0;
        double ringOffsetM = 0.0;      // set-back of the ring centerline inside the outline (> 0 = inward)
        double ringPassFraction = 0.0; // share of the ring cut by passes (between motus)
        struct Noise {
            double frequency = 0.0;
            double amplitude = 0.0; // relative to the island radius
            int octaves = 0;
            double lacunarity = 0.0;
            double gain = 0.0;
        } noise;
        double coastFalloffM = 0.0;
        double shelfWidthM = 0.0;
        double shelfDepthM = 0.0;
        double talusSlopeDeg = 0.0; // slope of the sea floor between the shelf edge and the boundary depth
        double talusWidthM = 0.0;   // horizontal extent of that slope
        struct SeafloorNoise {
            double frequency = 0.0;
            double amplitudeM = 0.0;
        } seafloorNoise;
    } shape;

    struct InitialTopography {
        std::string mode; // flat | peneplain | noise | volcanic_cone | atoll | barrier
        double baseElevationM = 0.0;
        struct Noise {
            double frequency = 0.0;
            double amplitudeM = 0.0;
            int octaves = 0;
            double lacunarity = 0.0;
            double gain = 0.0;
        } noise;
        struct Volcano {
            int count = 0;
            double heightM = 0.0;
            double radiusM = 0.0;
            double profileExponent = 0.0;
            double flankNoiseM = 0.0;         // irregular flanks (also deflects the lava flows)
            double flankNoiseFrequency = 0.0; // cycles per island radius
            int flankNoiseOctaves = 0;
            double craterRadiusM = 0.0;
            double craterDepthM = 0.0;
            double minSpacingM = 0.0; // between volcano centers
            bool caldera = false;
            double calderaRadiusM = 0.0;
            double calderaRimWidthM = 0.0; // width of the rounded crest between the caldera bowl and the flanks
            double summitIrregularity = 0.0; // relative variation of the crater / caldera radius around the vent
            double calderaDepthM = 0.0;
            int lavaFlowCount = 0;     // per volcano
            double lavaFlowHeightM = 0.0;
            double lavaFlowWidthM = 0.0;
            double lavaFlowTurnDeg = 0.0;     // random deviation from the steepest descent at each step
            double lavaFlowMinSlopeDeg = 0.0; // a flow stops on flatter ground (or at sea)
        } volcano;
        struct Atoll {
            bool makatea = false;     // uplifted atoll: a high ring with cliffs
            double ringHeightM = 0.0; // reef ring above sea level
            double ringNoiseM = 0.0;
            double lagoonDepthM = 0.0;
            double lagoonNoiseM = 0.0;
            int pinnacleCount = 0;
            double pinnacleRadiusM = 0.0;
            double pinnacleTopDepthM = 0.0; // pinnacles rise to this depth below the surface
        } atoll;
        struct Barrier {
            double crestHeightM = 0.0; // low ridge along the island, highest mid-width
        } barrier;
    } initialTopography;

    // Mountains made by noise: static zones of amplitude in [0, 1], each
    // multiplying a ridged multifractal noise.
    struct ReliefZone {
        std::string type; // range | massif | hills
        Vec2 center;
        double angleDeg = 0.0; // axis of the zone (u)
        double lengthM = 0.0;  // extent along the axis
        double widthM = 0.0;   // extent across it
        double heightM = 0.0;  // peak height added by the zone
    };
    struct Relief {
        std::vector<ReliefZone> zones;
        double warpAmplitude = 0.0; // domain warp of the zones, relative to R
        double modulation = 0.0;    // low-frequency variation of the zone amplitude
        struct MountainNoise {
            double frequency = 0.0; // cycles per island radius
            int octaves = 0;
            double lacunarity = 0.0;
            double gain = 0.0;
            double anisotropy = 0.0;    // stretch of the noise along a range's axis
            double warpAmplitude = 0.0; // domain warp of the noise, relative to its wavelength
        } mountainNoise;
    } relief;

    struct Material {
        double hardness = 0.0;
        double erodibility = 0.0;
        double talusAngleDeg = 0.0;
        std::string sediment;
    };
    struct Stratum {
        std::string material;
        double thicknessM = 0.0;
    };
    struct Geology {
        std::map<std::string, Material> materials;
        std::vector<Stratum> strata; // top to bottom
        double strataTopM = 0.0;
        std::string basementMaterial;
        struct Folding {
            bool enabled = false;
            double axisAngleDeg = 0.0;
            double wavelengthM = 0.0;
            double amplitudeM = 0.0;
            double asymmetry = 0.0;
            double plungeDeg = 0.0;
        } folding;
    } geology;

    // Particle-based hydraulic erosion (SimpleHydrology, Nick McDonald), in a
    // normalized frame: a cell is 1 unit wide, heights are divided by
    // heightScaleM and measured from sea level.
    struct Erosion {
        struct Particles {
            bool enabled = true;
            int steps = 0;
            int particlesPerStep = 0; // 0: automatic, land cells / 512
            int maxAge = 0;
            double gravity = 0.0;
            double momentumTransfer = 0.0;
            double entrainment = 0.0;
            double depositionRate = 0.0;
            double evaporationRate = 0.0;
            double minVolume = 0.0;
            double dischargeLrate = 0.0;   // learning rate of the time-averaged maps
            double dischargeScale = 0.0;   // discharge_norm = erf(scale * discharge)
            double hardnessInfluence = 0.0; // exponent of the rock erodibility
            double heightScaleM = 0.0;     // meters per normalized height unit (the cell size by default)
            int underwaterSteps = 0;       // iterations of a drop after it enters the sea
        } particles;
        struct Thermal {
            bool enabled = true;
            double rate = 0.0;
            double sedimentTalusAngleDeg = 0.0;
            double underwaterTalusAngleDeg = 0.0; // sediment below sea level
        } thermal;
    } erosion;

    // Rivers and lakes on the eroded terrain (step 4).
    struct Hydrology {
        bool enabled = true;
        double riverMinAreaM2 = 0.0;   // drained area where a river starts (its source)
        double riverWidthCoeff = 0.0;  // width = coeff * sqrt(drained area in m²)
        double riverMinWidthM = 0.0;
        double riverDepthRatio = 0.0;  // depth = ratio * width, at least 1 m
        double lakeMinDepthM = 0.0;
        double lakeMinAreaM2 = 0.0;
    } hydrology;

    // Conversion of the heightmap to blocks (step 6).
    struct Voxelization {
        bool enabled = true;
        int dirtDepthBlocks = 0;       // dirt under the grass on gentle slopes
        double grassMaxSlopeDeg = 0.0; // steeper surfaces are bare stone
    } voxelization;

    // Not used yet: will weight the rain and drive the dunes.
    struct Climate {
        double humidity = 0.0;
        struct Wind {
            double directionDeg = 0.0;
            double strength = 0.0;
            double variabilityDeg = 0.0;
        } wind;
    } climate;

    // Island radius R in meters: shape.radius_ratio * half the smaller map side.
    double islandRadiusM() const {
        const double sizeM = std::min(world.width, world.length) * world.blockSizeM;
        return shape.radiusRatio * sizeM / 2.0;
    }
};

enum class ParamSource : std::uint8_t {
    Seed,    // drawn from the seed
    Config,  // set in the config file
    Default, // fixed default (not random)
};

struct ParamEntry {
    std::string path; // e.g. "geology.strata[2].thickness_m"
    nlohmann::json value;
    ParamSource source;
};

struct ResolvedParams {
    IslandParams values;
    std::vector<ParamEntry> entries; // in resolution order
    nlohmann::json config;           // the input config, as given

    // The complete config with every value filled in (can be fed back as a
    // config to reproduce the exact same parameters).
    nlohmann::ordered_json toJson() const;
};

// Resolves every parameter of `config`: each one is drawn from the seed (from
// its own random stream, so values never shift when other parameters are
// overridden or added), then overridden by the config value if not null.
// Throws ConfigError on invalid values or unknown keys. `extraKeys` are
// top-level keys handled elsewhere (e.g. "pipeline").
ResolvedParams resolveIslandParams(std::uint64_t seed, const nlohmann::json& config,
                                   const std::vector<std::string>& extraKeys = {});

} // namespace wg
