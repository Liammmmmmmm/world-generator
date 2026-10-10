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
// Units: meters, years, degrees, °C. Noise frequencies are in cycles per island
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
        int coarseResolution = 0;
        double totalDurationYears = 0.0;
        double dtYears = 0.0;
        int climateUpdateIntervalSteps = 0;
        double boundaryDepthM = 0.0;
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
        double mainAxisDeg = 0.0;       // main axis of the island (also of the first collision and the folds)
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
        bool keepSatelliteIslets = true;
        double minIsletAreaM2 = 0.0;
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
            double calderaRimWidthM = 0.0; // width of the smoothstep wall down to the caldera floor
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
    // Fault trace centered on (x, y), along angle_deg. Its positive side, to the
    // left of the trace direction (normal (-sin, cos)), is raised by offset_m.
    struct Fault {
        double x = 0.0, y = 0.0;
        double angleDeg = 0.0;
        double lengthM = 0.0;
        double dipDeg = 0.0;
        double offsetM = 0.0;
        // -1: an old fault, offset from the start. Otherwise the tectonic phase it
        // bounds: its offset grows with that phase (0 before the simulation).
        int phase = -1;
    };
    struct Intrusion {
        std::string material;
        double x = 0.0, y = 0.0;
        double radiusM = 0.0;
        double topM = 0.0;
        double domeK = 0.0; // dome curvature: top drops by domeK * radius at the edge
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
        std::vector<Fault> faults;         // drawn, then those derived from the tectonic phases
        double faultTipTaper = 0.0;        // fault offsets fade over this fraction of the half-length at both tips
        std::vector<Intrusion> intrusions;
    } geology;

    struct TectonicPhase {
        std::string type; // collision | dome | horst_graben | block_uplift | tilt | subsidence
        double startYears = 0.0;
        double endYears = 0.0;
        double rateMPerYear = 0.0; // magnitude; the pattern carries the sign (subsidence is a negative dome)
        Vec2 center;
        double angleDeg = 0.0;
        double lengthM = 0.0;
        double widthM = 0.0;
        double falloff = 0.0;      // width of the soft edges, as a fraction of the half-size
        double faultDipDeg = 0.0;  // dip of the faults bounding a block_uplift / horst_graben
    };
    struct Tectonics {
        std::vector<TectonicPhase> phases;
        double maxUpliftRateMPerYear = 0.0;
    } tectonics;

    struct Climate {
        double baseTemperatureC = 0.0;
        double lapseRateCPerKm = 0.0;
        double humidity = 0.0;
        double basePrecipitationMPerYear = 0.0;
        struct Wind {
            double directionDeg = 0.0;
            double strength = 0.0;
            double variabilityDeg = 0.0;
        } wind;
        struct Swell {
            double directionDeg = 0.0;
            double energy = 0.0;
        } swell;
        struct Orographic {
            double condensationRate = 0.0;
            double rainShadowStrength = 0.0;
            double moistureRechargeOverSea = 0.0;
        } orographic;
    } climate;

    struct SeaLevelPoint {
        double tYears = 0.0;
        double levelM = 0.0;
    };
    struct Glaciation {
        double startYears = 0.0;
        double endYears = 0.0;
        double temperatureOffsetC = 0.0;
        double seaLevelDropM = 0.0; // used to derive sea_level_curve when it is not given
    };
    struct Timeline {
        std::vector<SeaLevelPoint> seaLevelCurve;
        std::vector<Glaciation> glaciations;
    } timeline;

    struct Erosion {
        struct Fluvial {
            double kBase = 0.0;
            double m = 0.0;
            double n = 0.0;
            double depositionG = 0.0;
            bool precipitationWeighted = true;
        } fluvial;
        struct Hillslope {
            double diffusivityM2PerYear = 0.0;
        } hillslope;
        struct Thermal {
            int iterationsPerStep = 0;
            double rate = 0.0;
        } thermal;
        struct Glacial {
            double accumulationRate = 0.0;
            double ablationGradient = 0.0;
            double deformationCoeff = 0.0;
            double slidingCoeff = 0.0;
            double erosionCoeff = 0.0;
            double erosionExponent = 0.0;
        } glacial;
        struct Coastal {
            double waveErosionCoeff = 0.0;
            int fetchRayCount = 0;
            double fetchMaxDistanceM = 0.0;
            double platformDepthM = 0.0;
            double sedimentExportThreshold = 0.0;
        } coastal;
        struct DetailHydraulic {
            std::string method; // droplet | pipe
            int dropletCount = 0;
            double inertia = 0.0;
            double capacity = 0.0;
            double erosionRate = 0.0;
            double depositionRate = 0.0;
            double evaporation = 0.0;
            int radius = 0;
            int maxLifetime = 0;
        } detailHydraulic;
        struct Aeolian {
            bool enabled = false;
            double sandSupply = 0.0;
            int iterations = 0;
            double slabHeightM = 0.0;
            double hopLengthM = 0.0;
            double depositProbSand = 0.0;
            double depositProbBare = 0.0;
            double reposeAngleDeg = 0.0;
            double shadowAngleDeg = 0.0;
        } aeolian;
    } erosion;

    struct Hydrology {
        double riverMinAreaM2 = 0.0;
        double widthCoeff = 0.0;
        double widthExponent = 0.0;
        double depthCoeff = 0.0;
        double depthExponent = 0.0;
        double lakeMinAreaM2 = 0.0;
        int waterfallMinDropBlocks = 0;
    } hydrology;

    struct PresentClimate {
        double temperatureOffsetC = 0.0;
        double glacierMinThicknessM = 0.0;
        double snowMinAltitudeOffsetM = 0.0;
    } presentClimate;

    struct Soil {
        double formationRate = 0.0; // m/year
        double maxDepthM = 0.0;
        double slopeLimitDeg = 0.0;
    } soil;

    struct Biomes {
        struct TransitionNoise {
            double frequency = 0.0;
            double amplitude = 0.0;
        } transitionNoise;
        nlohmann::json table; // null = built-in climate -> biome table
    } biomes;

    struct Voxelization {
        bool surfaceMaterialBySlope = true;
        double strataJitterM = 0.0;
        struct Overhangs {
            bool enabled = false;
            double minSlopeDeg = 0.0;
            double noiseFrequency = 0.0;
            int depthBlocks = 0;
        } overhangs;
        struct SeaNotches {
            bool enabled = false;
            double minExposure = 0.0;
            int heightBlocks = 0;
            int depthBlocks = 0;
        } seaNotches;
    } voxelization;

    struct Toggle {
        bool enabled = false;
    };
    Toggle caves;
    Toggle vegetation;
    Toggle structures;

    // Island radius R in meters: shape.radius_ratio * half the smaller map side.
    double islandRadiusM() const {
        const double sizeM = std::min(world.width, world.length) * world.blockSizeM;
        return shape.radiusRatio * sizeM / 2.0;
    }

    struct Debug {
        std::optional<std::string> stopAfterStage;
        std::vector<std::string> dumpStages;
        std::string cacheDir;
        std::optional<std::string> forceRecomputeFrom;
    } debug;
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
