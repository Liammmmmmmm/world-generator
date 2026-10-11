// Distribution of every island parameter.
//
// Ranges noted [a, b] in island_params.jsonc are used as given; the others are
// plausible values chosen here and can be tuned freely. Some distributions
// depend on already-resolved parameters (mostly origin.type and origin.age):
// they use the final value, so overriding origin.type also changes what is
// drawn for the parameters that depend on it, while the random streams
// themselves stay the same.

#include "wg/params/IslandParams.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "wg/core/Config.hpp"
#include "wg/island/UpliftPattern.hpp"
#include "wg/params/ParamResolver.hpp"

namespace wg {

namespace {

using Weights = ParamResolver::Weights;

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

double logLerp(double a, double b, double t) {
    return a * std::pow(b / a, t);
}

std::vector<std::string> materialNames(const IslandParams::Geology& geology) {
    std::vector<std::string> names;
    for (const auto& [name, _] : geology.materials)
        names.push_back(name);
    return names;
}

// Drops options that are not in the materials table (it can be overridden).
Weights knownMaterials(Weights weights, const IslandParams::Geology& geology) {
    std::erase_if(weights, [&](const auto& w) { return !geology.materials.contains(w.first); });
    return weights;
}

std::string at(const std::string& list, std::size_t i, const char* field) {
    return list + "[" + std::to_string(i) + "]." + field;
}

// Weights by origin type: volcanic, continental, sedimentary, coral.
int originIndex(const std::string& origin) {
    return origin == "volcanic" ? 0 : origin == "continental" ? 1 : origin == "sedimentary" ? 2 : 3;
}

// Value by origin type: volcanic, continental, sedimentary, coral.
template <typename T>
const T& perOrigin(const std::string& origin, const std::array<T, 4>& table) {
    return table[originIndex(origin)];
}

struct Range {
    double min, max;
    double at(double u) const { return min + (max - min) * u; }
};

Weights byOrigin(const std::string& origin, const std::array<Weights, 4>& table) {
    if (origin == "volcanic")
        return table[0];
    if (origin == "continental")
        return table[1];
    if (origin == "sedimentary")
        return table[2];
    return table[3];
}

const nlohmann::json kDefaultMaterials = {
    {"basalt", {{"hardness", 0.80}, {"erodibility", 0.6}, {"talus_angle_deg", 45}, {"sediment", "gravel"}}},
    {"granite", {{"hardness", 0.90}, {"erodibility", 0.4}, {"talus_angle_deg", 50}, {"sediment", "sand"}}},
    {"gneiss", {{"hardness", 0.85}, {"erodibility", 0.5}, {"talus_angle_deg", 48}, {"sediment", "sand"}}},
    {"schist", {{"hardness", 0.50}, {"erodibility", 1.2}, {"talus_angle_deg", 38}, {"sediment", "clay"}}},
    {"limestone", {{"hardness", 0.60}, {"erodibility", 1.0}, {"talus_angle_deg", 55}, {"sediment", "clay"}}},
    {"sandstone", {{"hardness", 0.45}, {"erodibility", 1.3}, {"talus_angle_deg", 42}, {"sediment", "sand"}}},
    {"shale", {{"hardness", 0.25}, {"erodibility", 2.0}, {"talus_angle_deg", 30}, {"sediment", "clay"}}},
    {"tuff", {{"hardness", 0.30}, {"erodibility", 1.8}, {"talus_angle_deg", 35}, {"sediment", "sand"}}},
};

std::map<std::string, IslandParams::Material> parseMaterials(const nlohmann::json& json) {
    std::map<std::string, IslandParams::Material> materials;
    for (const auto& [name, m] : json.items()) {
        const std::string path = "geology.materials." + name;
        try {
            materials[name] = {m.at("hardness").get<double>(), m.at("erodibility").get<double>(),
                               m.at("talus_angle_deg").get<double>(), m.at("sediment").get<std::string>()};
        } catch (const nlohmann::json::exception&) {
            throw ConfigError("'" + path +
                              "' must be an object with numeric hardness, erodibility, talus_angle_deg "
                              "and a string sediment");
        }
        const std::string& sediment = materials[name].sediment;
        if (sediment != "gravel" && sediment != "sand" && sediment != "clay")
            throw ConfigError("'" + path + ".sediment' must be \"gravel\", \"sand\" or \"clay\"");
        if (materials[name].erodibility < 0.0)
            throw ConfigError("'" + path + ".erodibility' must be >= 0");
    }
    return materials;
}

// --- Sections ----------------------------------------------------------------------

void resolveWorld(ParamResolver& r, IslandParams::World& w) {
    w.width = r.whole("world.width", 4096);
    w.length = r.whole("world.length", 4096);
    w.blockSizeM = r.number("world.block_size_m", 1.0);
    w.chunkSize = r.whole("world.chunk_size", 32);
    w.seaLevelY = r.whole("world.sea_level_y", 0);
    w.verticalExaggeration = r.number("world.vertical_exaggeration", 1.0);
    // Provisional vertical bounds of the voxel world; to be revisited with voxelization.
    w.minY = r.whole("world.min_y", -64);
    w.maxY = r.whole("world.max_y", 320);

    if (w.width <= 0 || w.length <= 0)
        throw ConfigError("'world.width' and 'world.length' must be positive");
    if (w.blockSizeM <= 0.0)
        throw ConfigError("'world.block_size_m' must be positive");
    if (w.minY >= w.maxY)
        throw ConfigError("'world.min_y' must be lower than 'world.max_y'");
    if (w.seaLevelY < w.minY || w.seaLevelY >= w.maxY)
        throw ConfigError("'world.sea_level_y' must be within [min_y, max_y)");
}

void resolveOrigin(ParamResolver& r, IslandParams::Origin& o) {
    const auto weights = r.object("origin.type_weights",
                                  {{"volcanic", 0.25}, {"continental", 0.45}, {"sedimentary", 0.2}, {"coral", 0.1}});
    Weights list;
    for (const std::string type : {"volcanic", "continental", "sedimentary", "coral"}) {
        const auto it = weights.find(type);
        if (it != weights.end() && !it->is_number())
            throw ConfigError("'origin.type_weights." + type + "' must be a number");
        list.emplace_back(type, it != weights.end() ? it->get<double>() : 0.0);
        o.typeWeights[type] = list.back().second;
    }
    if (weights.size() != list.size())
        throw ConfigError("'origin.type_weights' only accepts volcanic, continental, sedimentary and coral");
    o.type = r.pick("origin.type", list);
    o.age = r.uniform("origin.age", 0.0, 1.0);
}

void resolveSimulation(ParamResolver& r, IslandParams& p) {
    auto& s = p.simulation;
    const auto& origin = p.origin;
    s.coarseResolution = r.whole("simulation.coarse_resolution", 1024);
    // 0.2 to 2 Ma: longer for old islands, and for large ones (a small island is
    // worn down sooner); the size counts from R = 500 m to 2 km.
    const double size = std::clamp(std::log(p.islandRadiusM() / 500.0) / std::log(4.0), 0.0, 1.0);
    s.totalDurationYears = r.sample("simulation.total_duration_years", [&](double u) {
        return logLerp(2e5, 2e6, 0.5 * u + 0.25 * origin.age + 0.25 * size);
    });
    s.dtYears = r.logUniform("simulation.dt_years", 500, 1e4);
    s.climateUpdateIntervalSteps = r.whole("simulation.climate_update_interval_steps", 10);
    s.processes.uplift = r.flag("simulation.processes.uplift", true);
    s.processes.climate = r.flag("simulation.processes.climate", true);
    s.processes.glaciers = r.flag("simulation.processes.glaciers", true);
    s.processes.fluvial = r.flag("simulation.processes.fluvial", true);
    s.processes.hillslope = r.flag("simulation.processes.hillslope", true);
    s.processes.marine = r.flag("simulation.processes.marine", true);
    s.logIntervalSteps = r.whole("simulation.log_interval_steps", 100);
    if (s.logIntervalSteps < 1 || s.coarseResolution < 8)
        throw ConfigError("'simulation.log_interval_steps' must be >= 1 and 'simulation.coarse_resolution' >= 8");
    // boundary_depth_m depends on the island geometry: see resolveBathymetry.
}

// Width of the free ring of sea between the outermost possible coast and the
// map edge, in meters: the lobes fit in R, their smooth union adds up to a
// quarter of its width, and the domain warp pushes the coast out by up to
// amplitude * R.
double freeSeaWidthM(const IslandParams& p) {
    const double halfSize = std::min(p.world.width, p.world.length) * p.world.blockSizeM / 2.0;
    const double outermost =
        p.islandRadiusM() * (1.0 + p.shape.lobeBlend / 4.0 + p.shape.noise.amplitude / p.shape.elongation);
    return std::max(0.0, halfSize - outermost);
}

// Shape distributions depend on the origin, so that each kind of island is
// recognizable (columns: volcanic, continental, sedimentary, coral):
//   volcanic     1-2 lobes close together, round, narrow shelf, steep slope
//   continental  2-5 spread lobes, often elongated (60 %), medium shelf
//   sedimentary  2-4 lobes aligned on a long thin island, wide shallow shelf
//   coral        a single round lobe, a reef flat for shelf, very steep drop
void resolveShape(ParamResolver& r, IslandParams& p) {
    auto& s = p.shape;
    const std::string& origin = p.origin.type;
    // Coral islands are atolls: a reef ring around a lagoon.
    s.mode = r.pick("shape.mode", perOrigin<Weights>(origin, {{{{"blobs", 1.0}}, {{"blobs", 1.0}}, {{"blobs", 1.0}}, {{"ring", 1.0}}}}),
                    {"blobs", "ring"});
    s.radiusRatio = r.uniform("shape.radius_ratio", 0.4, 0.8);
    s.lobeCount = r.sampleInteger("shape.lobe_count", [&](double u) {
        const auto [min, max] = perOrigin<std::pair<int, int>>(origin, {{{1, 2}, {2, 5}, {2, 4}, {1, 1}}});
        return std::min(max, min + static_cast<int>(u * (max - min + 1)));
    });
    if (s.lobeCount < 1)
        throw ConfigError("'shape.lobe_count' must be >= 1");
    s.lobeSpread = r.sample("shape.lobe_spread", [&](double u) {
        return perOrigin<Range>(origin, {{{0.0, 0.3}, {0.4, 0.9}, {0.2, 0.5}, {0.0, 0.2}}}).at(u);
    });
    // Lobes small compared to their spread stay distinct (peninsulas, isthmuses);
    // large ones merge into a single mass.
    s.lobeRadiusMin = r.uniform("shape.lobe_radius_min", 0.25, 0.6);
    s.lobeRadiusMax = r.sample("shape.lobe_radius_max",
                               [&](double u) { return std::min(1.0, s.lobeRadiusMin + lerp(0.1, 0.5, u)); });
    if (s.lobeRadiusMin <= 0.0 || s.lobeRadiusMax < s.lobeRadiusMin)
        throw ConfigError("'shape.lobe_radius_min' and 'shape.lobe_radius_max' must satisfy 0 < min <= max");
    s.lobeElongationMax = r.sample("shape.lobe_elongation_max", [&](double u) {
        return perOrigin<Range>(origin, {{{1.0, 1.5}, {1.0, 2.0}, {1.0, 1.5}, {1.0, 1.3}}}).at(u);
    });
    s.lobeAlignment = r.sample("shape.lobe_alignment", [&](double u) {
        return perOrigin<Range>(origin, {{{0.0, 1.0}, {0.0, 1.0}, {0.8, 1.0}, {0.0, 1.0}}}).at(u);
    });
    s.mainAxisDeg = r.uniform("shape.main_axis_deg", 0.0, 180.0);
    s.elongation = r.sample("shape.elongation", [&](double u) {
        if (origin == "continental") // elongated 60 % of the time
            return u < 0.6 ? lerp(1.5, 2.5, u / 0.6) : lerp(1.0, 1.5, (u - 0.6) / 0.4);
        return perOrigin<Range>(origin, {{{1.0, 1.3}, {}, {3.0, 8.0}, {1.0, 1.5}}}).at(u);
    });
    s.lobeBlend = r.uniform("shape.lobe_blend", 0.0, 0.3);
    if (s.lobeElongationMax < 1.0 || s.elongation < 1.0 || s.lobeBlend < 0.0)
        throw ConfigError("'shape.lobe_elongation_max' and 'shape.elongation' must be >= 1, 'shape.lobe_blend' >= 0");
    // Bays stay occasional, and rare on volcanic, sedimentary and coral islands.
    s.bayCount = r.sampleInteger("shape.bay_count", [&](double u) {
        const auto [none, one] = perOrigin<std::pair<double, double>>(origin, {{{0.7, 1.0}, {0.5, 0.85}, {0.85, 1.0}, {1.0, 1.0}}});
        return u < none ? 0 : u < one ? 1 : 2;
    });
    s.bayRadiusMin = r.number("shape.bay_radius_min", 0.15);
    s.bayRadiusMax = r.number("shape.bay_radius_max", 0.35);
    s.bayPenetration = r.uniform("shape.bay_penetration", 0.3, 0.8);
    if (s.bayCount < 0 || s.bayRadiusMin <= 0.0 || s.bayRadiusMax < s.bayRadiusMin)
        throw ConfigError("'shape.bay_count' must be >= 0 and 0 < 'shape.bay_radius_min' <= 'shape.bay_radius_max'");
    s.ringWidthM = r.uniform("shape.ring_width_m", 80, 250);
    s.ringWidthNoise = r.uniform("shape.ring_width_noise", 0.3, 0.8);
    // Few octaves: an atoll has a handful of wide passes, not many small ones.
    s.ringNoiseFrequency = r.logUniform("shape.ring_noise_frequency", 1.5, 4.0);
    s.ringNoiseOctaves = r.integer("shape.ring_noise_octaves", 1, 3);
    // The outer edge of the reef stays close to the outline.
    s.ringOffsetM = r.sample("shape.ring_offset_m", [&](double u) { return s.ringWidthM * lerp(0.5, 1.0, u); });
    s.ringPassFraction = r.uniform("shape.ring_pass_fraction", 0.05, 0.3);
    if (s.ringWidthM <= 0.0 || s.ringWidthNoise < 0.0 || s.ringPassFraction < 0.0 || s.ringPassFraction >= 1.0)
        throw ConfigError("'shape.ring_width_m' must be > 0, 'shape.ring_width_noise' >= 0 and "
                          "'shape.ring_pass_fraction' in [0, 1)");
    s.noise.frequency = r.logUniform("shape.noise.frequency", 1.0, 4.0);
    // Relative to the island half-width (R / elongation), so thin islands keep
    // their shape; volcanoes and reefs have smooth outlines.
    s.noise.amplitude = r.sample("shape.noise.amplitude", [&](double u) {
        return perOrigin<Range>(origin, {{{0.05, 0.15}, {0.1, 0.3}, {0.1, 0.3}, {0.02, 0.06}}}).at(u);
    });
    s.noise.octaves = r.integer("shape.noise.octaves", 3, 6);
    s.noise.lacunarity = r.number("shape.noise.lacunarity", 2.0);
    s.noise.gain = r.number("shape.noise.gain", 0.5);
    s.coastFalloffM = r.uniform("shape.coast_falloff_m", 50, 400);
    // The shelf takes a share of the free sea ring, so the slope still fits
    // after it; a coral reef flat is only a few tens of meters wide.
    s.shelfWidthM = r.sample("shape.shelf_width_m", [&](double u) {
        if (origin == "coral")
            return lerp(10, 50, u);
        const Range share = perOrigin<Range>(origin, {{{0.03, 0.1}, {0.1, 0.35}, {0.3, 0.6}, {}}});
        return std::clamp(freeSeaWidthM(p) * share.at(u), 10.0, 1500.0);
    });
    s.shelfDepthM = r.sample("shape.shelf_depth_m", [&](double u) {
        return perOrigin<Range>(origin, {{{-40, -10}, {-60, -10}, {-15, -3}, {-15, -5}}}).at(u);
    });
    s.seafloorNoise.frequency = r.logUniform("shape.seafloor_noise.frequency", 0.5, 2.0);
    s.seafloorNoise.amplitudeM = r.uniform("shape.seafloor_noise.amplitude_m", 5, 40);
    s.keepSatelliteIslets = r.flag("shape.keep_satellite_islets", true);
    s.minIsletAreaM2 = r.number("shape.min_islet_area_m2", 2000);
}

// The sea floor goes from the shelf edge down to the boundary depth with a
// plausible slope, and must reach it before the map edge: the boundary depth
// is limited by the room left, and the slope width follows from both.
void resolveBathymetry(ParamResolver& r, IslandParams& p) {
    auto& s = p.shape;
    s.talusSlopeDeg = r.sample("shape.talus_slope_deg", [&](double u) {
        return perOrigin<Range>(p.origin.type, {{{15, 30}, {8, 20}, {3, 10}, {35, 50}}}).at(u);
    });
    const double tanSlope = std::tan(s.talusSlopeDeg * std::numbers::pi / 180.0);
    p.simulation.boundaryDepthM = r.sample("simulation.boundary_depth_m", [&](double u) {
        const double maxDrop = std::max(0.0, freeSeaWidthM(p) - s.shelfWidthM) * tanSlope;
        return std::max(lerp(-1500, -200, u), s.shelfDepthM - maxDrop);
    });
    if (p.simulation.boundaryDepthM > s.shelfDepthM)
        throw ConfigError("'simulation.boundary_depth_m' must be deeper than 'shape.shelf_depth_m'");
    s.talusWidthM = r.sample("shape.talus_width_m", [&](double) {
        return std::max(1.0, (s.shelfDepthM - p.simulation.boundaryDepthM) / tanSlope);
    });
}

// Each origin has its own initial relief (columns: volcanic, continental,
// sedimentary, coral): volcanic cones, a low peneplain or noisy relief, a low
// barrier island with a crest, an atoll ring around its lagoon. Sizes are
// relative to the island so that proportions stay physical.
void resolveInitialTopography(ParamResolver& r, IslandParams& p) {
    auto& t = p.initialTopography;
    const auto& origin = p.origin;
    const double R = p.islandRadiusM();
    t.mode = r.pick("initial_topography.mode",
                    byOrigin(origin.type, {{
                                              {{"volcanic_cone", 1.0}},
                                              {{"peneplain", 0.5}, {"noise", 0.5}},
                                              {{"barrier", 1.0}},
                                              {{"atoll", 1.0}},
                                          }}),
                    {"flat", "peneplain", "noise", "volcanic_cone", "atoll", "barrier"});
    t.baseElevationM = r.sample("initial_topography.base_elevation_m", [&](double u) {
        if (t.mode == "volcanic_cone")
            return lerp(0, 30, u); // plateau under the cones
        if (t.mode == "barrier")
            return lerp(1, 3, u);
        if (t.mode == "atoll")
            return 0.0; // the ring height is initial_topography.atoll.ring_height_m
        return 10.0 + 290.0 * u * u; // low most of the time
    });
    t.noise.frequency = r.logUniform("initial_topography.noise.frequency", 1.0, 8.0);
    t.noise.amplitudeM = r.sample("initial_topography.noise.amplitude_m", [&](double u) {
        if (t.mode == "peneplain")
            return lerp(5, 40, u);
        if (t.mode == "barrier")
            return lerp(0.3, 1.5, u);
        return lerp(10, 200, u);
    });
    t.noise.octaves = r.integer("initial_topography.noise.octaves", 3, 7);
    t.noise.lacunarity = r.number("initial_topography.noise.lacunarity", 2.0);
    t.noise.gain = r.number("initial_topography.noise.gain", 0.5);
    // No flat land at the start: a regional dome (not for atolls and barriers,
    // flat by nature) and a multi-scale roughness.
    const bool flatByNature = t.mode == "atoll" || t.mode == "barrier";
    t.regionalDomeM = r.sample("initial_topography.regional_dome_m", [&](double u) {
        return perOrigin<Range>(origin.type, {{{5, 15}, {10, 30}, {5, 15}, {5, 10}}}).at(u);
    });
    t.roughnessM = r.sample("initial_topography.roughness_m", [&](double u) {
        return flatByNature ? lerp(0.5, 1.0, u) : perOrigin<Range>(origin.type, {{{3, 8}, {4, 10}, {2, 6}, {2, 4}}}).at(u);
    });
    t.roughnessFrequency = r.uniform("initial_topography.roughness_frequency", 4.0, 8.0);
    t.roughnessOctaves = r.integer("initial_topography.roughness_octaves", 4, 6);

    auto& v = t.volcano;
    v.count = r.integer("initial_topography.volcano.count", 1, 3);
    // The cone fills the island; its height keeps flanks of a few tens of degrees.
    v.radiusM = r.sample("initial_topography.volcano.radius_m", [&](double u) { return R * lerp(0.6, 1.0, u); });
    v.heightM = r.sample("initial_topography.volcano.height_m", [&](double u) { return v.radiusM * lerp(0.15, 0.45, u); });
    v.profileExponent = r.uniform("initial_topography.volcano.profile_exponent", 1.0, 3.0);
    // Irregular flanks, enough to bend the lava flows and give erosion something
    // to work on. The amplitude follows the wavelength (R / frequency), which
    // bounds the slopes of the noise: a fixed amplitude at a fine wavelength
    // makes needles.
    v.flankNoiseFrequency = r.logUniform("initial_topography.volcano.flank_noise_frequency", 4.0, 10.0);
    v.flankNoiseOctaves = r.integer("initial_topography.volcano.flank_noise_octaves", 2, 3);
    v.flankNoiseM = r.sample("initial_topography.volcano.flank_noise_m",
                             [&](double u) { return R / v.flankNoiseFrequency * lerp(0.03, 0.08, u); });
    v.craterRadiusM =
        r.sample("initial_topography.volcano.crater_radius_m", [&](double u) { return v.radiusM * lerp(0.05, 0.15, u); });
    v.craterDepthM =
        r.sample("initial_topography.volcano.crater_depth_m", [&](double u) { return v.heightM * lerp(0.05, 0.15, u); });
    v.minSpacingM =
        r.sample("initial_topography.volcano.min_spacing_m", [&](double u) { return v.radiusM * lerp(0.5, 1.2, u); });
    // Calderas form late in a volcano's life.
    v.caldera = r.chance("initial_topography.volcano.caldera", 0.2 + 0.4 * origin.age);
    v.calderaRadiusM =
        r.sample("initial_topography.volcano.caldera_radius_m", [&](double u) { return v.radiusM * lerp(0.15, 0.3, u); });
    v.calderaRimWidthM = r.sample("initial_topography.volcano.caldera_rim_width_m",
                                  [&](double u) { return v.calderaRadiusM * lerp(0.1, 0.3, u); });
    v.calderaDepthM =
        r.sample("initial_topography.volcano.caldera_depth_m", [&](double u) { return v.heightM * lerp(0.1, 0.3, u); });
    v.lavaFlowCount = r.integer("initial_topography.volcano.lava_flow_count", 0, 8);
    v.lavaFlowHeightM = r.uniform("initial_topography.volcano.lava_flow_height_m", 5, 15);
    v.lavaFlowWidthM = r.uniform("initial_topography.volcano.lava_flow_width_m", 10, 40);
    v.lavaFlowTurnDeg = r.uniform("initial_topography.volcano.lava_flow_turn_deg", 10, 40);
    v.lavaFlowMinSlopeDeg = r.uniform("initial_topography.volcano.lava_flow_min_slope_deg", 1, 3);

    auto& a = t.atoll;
    a.makatea = r.chance("initial_topography.atoll.makatea", 0.15);
    a.ringHeightM = r.sample("initial_topography.atoll.ring_height_m",
                             [&](double u) { return a.makatea ? lerp(30, 60, u) : lerp(1, 4, u); });
    a.ringNoiseM = r.sample("initial_topography.atoll.ring_noise_m",
                            [&](double u) { return a.ringHeightM * lerp(0.1, 0.3, u); });
    a.lagoonDepthM = r.uniform("initial_topography.atoll.lagoon_depth_m", 5, 40);
    a.lagoonNoiseM = r.sample("initial_topography.atoll.lagoon_noise_m",
                              [&](double u) { return a.lagoonDepthM * lerp(0.05, 0.2, u); });
    a.pinnacleCount = r.integer("initial_topography.atoll.pinnacle_count", 3, 15);
    a.pinnacleRadiusM = r.uniform("initial_topography.atoll.pinnacle_radius_m", 10, 40);
    a.pinnacleTopDepthM = r.uniform("initial_topography.atoll.pinnacle_top_depth_m", 1, 2);

    t.barrier.crestHeightM = r.uniform("initial_topography.barrier.crest_height_m", 1, 5);

    if (v.heightM < 0.0 || v.radiusM <= 0.0 || v.craterRadiusM <= 0.0 || v.calderaRadiusM <= 0.0 ||
        v.lavaFlowWidthM <= 0.0 || a.pinnacleRadiusM <= 0.0)
        throw ConfigError("'initial_topography.volcano' and 'initial_topography.atoll' sizes must be positive");
}

void resolveGeology(ParamResolver& r, IslandParams::Geology& g, const IslandParams::Origin& origin, double radius,
                    double mainAxisDeg, const IslandParams::Tectonics& tectonics) {
    g.materials = parseMaterials(r.object("geology.materials", kDefaultMaterials));
    const auto materials = materialNames(g);

    const auto strataMaterials = byOrigin(origin.type, {{
                                                           {{"basalt", 0.5}, {"tuff", 0.4}, {"limestone", 0.1}},
                                                           {{"schist", 0.25}, {"gneiss", 0.2}, {"sandstone", 0.2},
                                                            {"shale", 0.15}, {"limestone", 0.2}},
                                                           // sand and clay are sediments: their rocks here
                                                           {{"sandstone", 0.5}, {"shale", 0.35}, {"limestone", 0.15}},
                                                           {{"limestone", 1.0}},
                                                       }});
    const int strataCount = r.listSize("geology.strata", "geology.strata_count", 3, 12);
    for (int i = 0; i < strataCount; ++i) {
        IslandParams::Stratum stratum;
        stratum.material = r.pick(at("geology.strata", i, "material"), knownMaterials(strataMaterials, g), materials);
        stratum.thicknessM = r.logUniform(at("geology.strata", i, "thickness_m"), 10, 200);
        g.strata.push_back(stratum);
    }
    g.strataTopM = r.sample("geology.strata_top_m",
                            [&](double u) { return origin.type == "coral" ? lerp(5, 80, u) : lerp(50, 600, u); });
    g.basementMaterial = r.pick("geology.basement_material", knownMaterials(byOrigin(origin.type, {{
                                                                          {{"basalt", 1.0}},
                                                                          {{"granite", 0.6}, {"gneiss", 0.4}},
                                                                          {{"schist", 0.5}, {"granite", 0.5}},
                                                                          {{"basalt", 1.0}},
                                                                      }}), g),
                                materials);

    auto& f = g.folding;
    const double foldingChance = origin.type == "continental"   ? 0.6
                                 : origin.type == "sedimentary" ? 0.5
                                 : origin.type == "volcanic"    ? 0.1
                                                                : 0.05;
    f.enabled = r.chance("geology.folding.enabled", foldingChance);
    // Continental folds follow the main axis of the island (the collision that made them).
    f.axisAngleDeg = r.sample("geology.folding.axis_angle_deg",
                              [&](double u) { return origin.type == "continental" ? mainAxisDeg : 180.0 * u; });
    // Folds a few hundred meters to a couple of kilometers wide, with an
    // amplitude of 3 to 20 % of the wavelength (beyond, limbs get near vertical).
    f.wavelengthM = r.sample("geology.folding.wavelength_m", [&](double u) { return radius * logLerp(0.4, 1.5, u); });
    f.amplitudeM = r.sample("geology.folding.amplitude_m", [&](double u) { return f.wavelengthM * lerp(0.03, 0.2, u); });
    f.asymmetry = r.uniform("geology.folding.asymmetry", 0, 1);
    f.plungeDeg = r.uniform("geology.folding.plunge_deg", 0, 20);

    const int maxFaults = origin.type == "continental" ? 6 : origin.type == "coral" ? 1 : 4;
    const int faultCount = r.listSize("geology.faults", "geology.fault_count", 0, maxFaults);
    for (int i = 0; i < faultCount; ++i) {
        IslandParams::Fault fault;
        fault.x = r.uniform(at("geology.faults", i, "x"), -radius, radius);
        fault.y = r.uniform(at("geology.faults", i, "y"), -radius, radius);
        fault.angleDeg = r.uniform(at("geology.faults", i, "angle_deg"), 0, 180);
        fault.lengthM = r.sample(at("geology.faults", i, "length_m"), [&](double u) { return radius * logLerp(0.3, 2.0, u); });
        fault.dipDeg = r.uniform(at("geology.faults", i, "dip_deg"), 45, 90);
        fault.offsetM = r.logUniform(at("geology.faults", i, "offset_m"), 5, 150);
        g.faults.push_back(fault);
    }

    g.faultTipTaper = r.uniform("geology.fault_tip_taper", 0.1, 0.4);

    // Continental intrusions (granite plutons) sit preferably in the collision zone.
    const auto collision = std::find_if(tectonics.phases.begin(), tectonics.phases.end(),
                                        [](const auto& phase) { return phase.type == "collision"; });
    const bool inCollision = origin.type == "continental" && collision != tectonics.phases.end();
    const int maxIntrusions = origin.type == "volcanic" || origin.type == "continental" ? 3 : 1;
    const int intrusionCount = r.listSize("geology.intrusions", "geology.intrusion_count", 0, maxIntrusions);
    for (int i = 0; i < intrusionCount; ++i) {
        IslandParams::Intrusion intrusion;
        // Volcanic islands: basaltic plugs (granite is rare on oceanic volcanoes);
        // elsewhere mostly granite plutons.
        intrusion.material = r.pick(at("geology.intrusions", i, "material"),
                                    knownMaterials(origin.type == "volcanic" ? Weights{{"basalt", 1.0}}
                                                                             : Weights{{"granite", 0.8}, {"basalt", 0.2}},
                                                   g),
                                    materials);
        if (inCollision) { // within the collision belt: along its axis, close to its crest
            const double a = collision->angleDeg * std::numbers::pi / 180.0;
            const double u = collision->lengthM * (r.unit(at("geology.intrusions", i, "x")) - 0.5) * 0.8;
            const double v = collision->widthM * (r.unit(at("geology.intrusions", i, "y")) - 0.5);
            intrusion.x = r.sample(at("geology.intrusions", i, "x"),
                                   [&](double) { return collision->center.x + u * std::cos(a) - v * std::sin(a); });
            intrusion.y = r.sample(at("geology.intrusions", i, "y"),
                                   [&](double) { return collision->center.y + u * std::sin(a) + v * std::cos(a); });
        } else {
            intrusion.x = r.uniform(at("geology.intrusions", i, "x"), -0.6 * radius, 0.6 * radius);
            intrusion.y = r.uniform(at("geology.intrusions", i, "y"), -0.6 * radius, 0.6 * radius);
        }
        intrusion.radiusM = r.sample(at("geology.intrusions", i, "radius_m"), [&](double u) { return radius * lerp(0.1, 0.4, u); });
        // Altitude of the top: on old continental crust, eroded plutons may already
        // outcrop; elsewhere they are still buried (erosion may expose them later).
        intrusion.topM = r.sample(at("geology.intrusions", i, "top_m"), [&](double u) {
            return perOrigin<Range>(origin.type, {{{-200, 50}, {-100, 300}, {-300, 0}, {-300, 0}}}).at(u);
        });
        intrusion.domeK = r.number(at("geology.intrusions", i, "dome_k"), 0.5);
        g.intrusions.push_back(intrusion);
    }
}

// Faults along the edges of block_uplift and horst_graben phases, raising the
// uplifted side by the phase's total uplift. These are the nominal walls,
// listed for information: the geology moves the blocks between walls that
// follow the warped outline of the pattern (UpliftPattern), which these
// straight traces only approximate.
std::vector<IslandParams::Fault> phaseFaults(const IslandParams::TectonicPhase& phase, double radius) {
    std::vector<IslandParams::Fault> faults;
    const double axis = phase.angleDeg * std::numbers::pi / 180.0;
    const double ux = std::cos(axis), uy = std::sin(axis), vx = -uy, vy = ux;
    const double uplift = phase.rateMPerYear * (phase.endYears - phase.startYears);
    // A trace at (u, v) in the phase frame, `angle` relative to the axis.
    const auto add = [&](double u, double v, double angleDeg, double length, double offset) {
        faults.push_back({phase.center.x + u * ux + v * vx, phase.center.y + u * uy + v * vy,
                          std::fmod(phase.angleDeg + angleDeg + 360.0, 360.0), length, phase.faultDipDeg, offset});
    };
    if (phase.type == "block_uplift") {
        // Positive (raised) side towards the inside of the block on all four edges.
        add(0, phase.widthM / 2, 180, phase.lengthM, uplift);
        add(0, -phase.widthM / 2, 0, phase.lengthM, uplift);
        add(phase.lengthM / 2, 0, 90, phase.widthM, uplift);
        add(-phase.lengthM / 2, 0, -90, phase.widthM, uplift);
    } else if (phase.type == "horst_graben") {
        // Band k covers v in [k w, (k+1) w], a horst (+1) for even k; the horst
        // side of a boundary is raised by (1 + negative_factor) * uplift. Only boundaries
        // whose trace crosses the island (within R of the map center) are kept.
        const int reach = static_cast<int>(std::ceil(2.0 * radius / phase.widthM));
        for (int k = -reach; k <= reach; ++k) {
            add(0, k * phase.widthM, k % 2 == 0 ? 0 : 180, phase.lengthM, (1.0 + phase.negativeFactor) * uplift);
            const auto& f = faults.back();
            const double a = f.angleDeg * std::numbers::pi / 180.0;
            const double along = std::clamp(-(f.x * std::cos(a) + f.y * std::sin(a)), -f.lengthM / 2, f.lengthM / 2);
            if (std::hypot(f.x + along * std::cos(a), f.y + along * std::sin(a)) > radius)
                faults.pop_back();
        }
    }
    return faults;
}

// Rough mean initial altitude of the land (m), from the initial topography
// parameters (the actual relief is only built by the island initialization).
double meanInitialElevation(const IslandParams& p) {
    const auto& t = p.initialTopography;
    if (t.mode == "atoll")
        return t.atoll.ringHeightM;
    if (t.mode == "barrier")
        return t.baseElevationM + 0.5 * t.barrier.crestHeightM;
    const double dome = 0.4 * t.regionalDomeM; // mean of the dome over the island
    if (t.mode == "volcanic_cone")
        return t.baseElevationM + 0.25 * t.volcano.heightM + dome;
    return t.baseElevationM + dome;
}

// Mean over the island of each phase's total uplift (rate x duration x
// pattern), in meters. The island is approximated by the ellipse of its shape
// parameters: half-length R along the main axis, R / elongation across.
std::vector<double> meanPhaseUplift(const IslandParams& p) {
    constexpr int kSamples = 48;
    const double R = p.islandRadiusM();
    const double axis = p.shape.mainAxisDeg * std::numbers::pi / 180.0;
    std::vector<double> means;
    for (std::size_t k = 0; k < p.tectonics.phases.size(); ++k) {
        const auto& phase = p.tectonics.phases[k];
        const UpliftPattern pattern(p, static_cast<int>(k));
        double sum = 0.0;
        int count = 0;
        for (int j = 0; j < kSamples; ++j) {
            for (int i = 0; i < kSamples; ++i) {
                const double u = (2.0 * (i + 0.5) / kSamples - 1.0), v = (2.0 * (j + 0.5) / kSamples - 1.0);
                if (u * u + v * v > 1.0)
                    continue;
                const double along = u * R, across = v * R / p.shape.elongation;
                sum += pattern.value(along * std::cos(axis) - across * std::sin(axis),
                                     along * std::sin(axis) + across * std::cos(axis));
                ++count;
            }
        }
        means.push_back(phase.rateMPerYear * (phase.endYears - phase.startYears) * sum / std::max(count, 1));
    }
    return means;
}

// Tectonics by origin: volcanic islands get at most a weak dome, sedimentary
// and coral islands at most a weak subsidence, continental ones mostly
// collisions and domes, never a subsidence (unless forced). Block and horst
// edges are narrow, but never under 10 % of the phase width.
// Steady-state relief (m) per unit of U / K at P = 1 m/yr, m = 0.45, for an
// island of R ~ 1.2 km on cells of 4 m: `wgen --calibrate --resolution 1024`
// gives a 99th percentile of the land altitude of 9.1 to 10.5 times U / K
// over U in [1e-4, 1e-3] and k_base in [1e-6, 3e-5].
constexpr double kSteadyReliefM = 10.0;

void resolveTectonics(ParamResolver& r, IslandParams& p) {
    auto& t = p.tectonics;
    const auto& origin = p.origin;
    const double duration = p.simulation.totalDurationYears;
    const double radius = p.islandRadiusM();
    // Uplift follows the erodibility. The steady-state relief of rivers under
    // an uplift U is about kSteadyReliefM * (U / K) * P^-m (wgen --calibrate),
    // so independent draws of U and k_base, over 2 and 3 orders of magnitude,
    // gave reliefs from a few meters (the island worn flat within a few
    // hundred ka) to kilometers. A relief is drawn instead (young islands are
    // higher), and the maximum rate derived from it. With a low K the steady
    // state takes 5 to 15 Ma (longer than any run): the rate must then also
    // build that relief within the run, U >= relief / duration.
    // (The hillslopes, absent from the calibration, then lower the actual
    // relief by a factor of 2 to 5: slopes are capped at their repose angle.)
    t.targetReliefM = r.sample("tectonics.target_relief_m", [&](double u) {
        const double v = (u + 1.0 - origin.age) / 2.0;
        // A volcano: about the height of its cone (a growing one keeps its size).
        return origin.type == "continental" ? logLerp(150, 1500, v)
               : origin.type == "volcanic"  ? p.initialTopography.volcano.heightM * lerp(0.5, 1.0, v)
                                            : logLerp(5, 30, v);
    });
    const auto& fluvial = p.erosion.fluvial;
    const double steadyReliefPerUK = kSteadyReliefM * std::pow(p.climate.basePrecipitationMPerYear, -fluvial.m);
    t.maxUpliftRateMPerYear = r.sample("tectonics.max_uplift_rate_m_per_year", [&](double) {
        const double rate = t.targetReliefM * std::max(fluvial.kBase / steadyReliefPerUK, 1.0 / duration);
        return std::clamp(rate, 1e-5, 1e-2);
    });
    t.upliftWarpAmplitude = r.uniform("tectonics.uplift_warp_amplitude", 0.1, 0.25);
    t.upliftWarpFrequency = r.uniform("tectonics.uplift_warp_frequency", 0.7, 1.5);
    t.upliftModulation = r.uniform("tectonics.uplift_modulation", 0.3, 0.6);
    t.upliftModulationFrequency = r.number("tectonics.uplift_modulation_frequency", 2.0);

    const auto types = byOrigin(origin.type, {{
                                                 {{"dome", 1.0}},
                                                 {{"collision", 0.4}, {"dome", 0.3}, {"tilt", 0.1},
                                                  {"block_uplift", 0.1}, {"horst_graben", 0.1}},
                                                 {{"subsidence", 1.0}},
                                                 {{"subsidence", 1.0}},
                                             }});
    // Most continental islands are still rising: their last uplift phase lasts
    // to the end (it starts in the first fifth of the run). So are young
    // volcanoes (a dome: magmatic inflation and new flows). The others are
    // "old" islands, worn down once their phases are over.
    const double ongoingChance = origin.type == "continental" ? 0.75 : origin.type == "volcanic" ? 1.0 - origin.age : 0.0;
    t.ongoingUplift = r.chance("tectonics.ongoing_uplift", ongoingChance);
    auto [minCount, maxCount] = perOrigin<std::pair<int, int>>(origin.type, {{{0, 1}, {1, 4}, {0, 1}, {0, 1}}});
    if (t.ongoingUplift)
        minCount = std::max(minCount, 1); // a growing volcano has its dome
    const Range strength = perOrigin<Range>(origin.type, {{{0.05, 0.3}, {0.2, 1.0}, {0.05, 0.3}, {0.05, 0.3}}});
    // A subsidence must not sink more than half of the island's mean initial
    // altitude (low islands would drown under the slightest one).
    const double maxSubsidenceM = 0.5 * std::max(meanInitialElevation(p), 0.0);
    // Diameter of the island (the ellipse of meanPhaseUplift) along a direction.
    const auto diameter = [&](double angleDeg) {
        const double a = (angleDeg - p.shape.mainAxisDeg) * std::numbers::pi / 180.0;
        return 2.0 * std::hypot(radius * std::cos(a), radius / p.shape.elongation * std::sin(a));
    };
    const int count = r.listSize("tectonics.phases", "tectonics.phase_count", minCount, maxCount);
    std::vector<std::string> phaseTypes;
    for (int i = 0; i < count; ++i)
        phaseTypes.push_back(r.pick(at("tectonics.phases", i, "type"), types,
                                    {"collision", "dome", "horst_graben", "block_uplift", "tilt", "subsidence"}));
    int ongoing = -1;
    for (int i = 0; i < count; ++i)
        if (phaseTypes[i] == "collision" || phaseTypes[i] == "dome" || phaseTypes[i] == "block_uplift")
            ongoing = t.ongoingUplift ? i : -1;
    bool collisionSeen = false;
    for (int i = 0; i < count; ++i) {
        const std::string list = "tectonics.phases";
        IslandParams::TectonicPhase phase;
        phase.type = phaseTypes[i];
        phase.startYears = r.sample(at(list, i, "start_years"), [&](double u) {
            return (i == ongoing ? 0.2 : 0.8) * duration * u;
        });
        phase.endYears = r.sample(at(list, i, "end_years"), [&](double u) {
            return i == ongoing ? duration : std::min(duration, phase.startYears + lerp(0.1, 0.6, u) * duration);
        });
        phase.rateMPerYear = r.sample(at(list, i, "rate_m_per_year"), [&](double u) {
            // the ongoing phase is the main active one
            const double rate = t.maxUpliftRateMPerYear * (i == ongoing ? lerp(0.6, 1.0, u) : strength.at(u));
            const double span = phase.endYears - phase.startYears;
            if (phase.type == "subsidence" && origin.type != "continental" && span > 0.0)
                return std::min(rate, maxSubsidenceM / span);
            return rate;
        });
        phase.center = r.point(at(list, i, "center"), -0.6 * radius, 0.6 * radius, -0.6 * radius, 0.6 * radius);
        // The first collision builds the island's main axis.
        const bool mainCollision = phase.type == "collision" && !collisionSeen;
        collisionSeen = collisionSeen || phase.type == "collision";
        phase.angleDeg = r.sample(at(list, i, "angle_deg"), [&](double u) { return mainCollision ? p.shape.mainAxisDeg : 180.0 * u; });
        // At most 1.2 times the island's diameter in each direction.
        phase.lengthM = r.sample(at(list, i, "length_m"), [&](double u) {
            return std::min(radius * lerp(0.8, 2.0, u), 1.2 * diameter(phase.angleDeg));
        });
        phase.widthM = r.sample(at(list, i, "width_m"), [&](double u) {
            return std::min(radius * lerp(0.2, 0.8, u), 1.2 * diameter(phase.angleDeg + 90.0));
        });
        const bool sharp = phase.type == "block_uplift" || phase.type == "horst_graben";
        phase.falloff = r.sample(at(list, i, "falloff"), [&](double u) { return sharp ? lerp(0.2, 0.35, u) : lerp(0.2, 1.0, u); });
        phase.faultDipDeg = r.uniform(at(list, i, "fault_dip_deg"), 55, 80);
        // A tilt or a horst_graben raises more than it lowers: the island must
        // not sink because of a tilt.
        phase.negativeFactor = r.number(at(list, i, "negative_factor"), 0.3);
        if (phase.rateMPerYear < 0.0 || phase.widthM <= 0.0 || phase.lengthM <= 0.0 || phase.falloff <= 0.0 ||
            phase.negativeFactor < 0.0)
            throw ConfigError("'" + at(list, i, "") + "': rate and negative_factor must be >= 0, width, length and falloff > 0");
        t.phases.push_back(phase);
    }

    // Check: the island must not sink on average. While the mean total uplift
    // over the island is below the allowed sinking (none for a continental
    // island, the capped subsidence for the others), the phase that lowers it
    // most is dropped (never one whose type the config forces).
    const double allowedSinkingM = origin.type == "continental" ? 0.0 : maxSubsidenceM;
    for (std::vector<double> means = meanPhaseUplift(p);;) {
        double total = 0.0;
        for (double m : means)
            total += m;
        if (total >= -allowedSinkingM - 1e-9)
            break;
        int worst = -1;
        for (std::size_t k = 0; k < means.size(); ++k)
            if (means[k] < 0.0 && r.source(at("tectonics.phases", k, "type")) != ParamSource::Config &&
                (worst < 0 || means[k] < means[worst]))
                worst = static_cast<int>(k);
        if (worst < 0) {
            r.note("tectonics: the island sinks on average (" + std::to_string(total) +
                   " m of uplift), kept: the config forces the lowering phases");
            break;
        }
        r.note("tectonics: phase " + std::to_string(worst) + " (" + t.phases[worst].type +
               ") dropped: the mean uplift over the island was " + std::to_string(total) + " m, " +
               std::to_string(means[worst]) + " m from that phase");
        t.phases.erase(t.phases.begin() + worst);
        r.removeListElement("tectonics.phases", "tectonics.phase_count", worst);
        means = meanPhaseUplift(p); // the following phases changed index, so their noises
    }

    // Derived faults: always recomputed from the phases (a "geology.derived_faults"
    // key in the config is ignored), listed for display only.
    r.markConsumed("geology.derived_faults");
    int derived = 0;
    for (std::size_t k = 0; k < t.phases.size(); ++k) {
        for (auto fault : phaseFaults(t.phases[k], radius)) {
            fault.phase = static_cast<int>(k); // offset grows with the phase (step 3)
            const std::string path = "geology.derived_faults[" + std::to_string(derived++) + "]";
            r.record(path, {{"x", fault.x}, {"y", fault.y}, {"angle_deg", fault.angleDeg}, {"length_m", fault.lengthM},
                            {"dip_deg", fault.dipDeg}, {"offset_m", fault.offsetM}, {"phase", k}},
                     ParamSource::Seed);
            p.geology.faults.push_back(fault);
        }
    }
}

// Multipliers of the process coefficients by origin: sedimentary islands
// (soft, flat) erode less by rivers and more at the coast, where the sea
// carries their sand further; coral islands hardly have rivers. Both are low
// sand islands, which the hillslope diffusion would spread into the sea within
// a few hundred ka: the waves and the wind that keep feeding them sand are not
// modelled, so their diffusion is reduced.
void resolveProcessMultipliers(ParamResolver& r, IslandParams& p) {
    struct Defaults {
        const char* origin;
        IslandParams::ProcessMultipliers values;
    };
    const Defaults table[] = {
        {"volcanic", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
        {"continental", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
        {"sedimentary", {0.5, 0.2, 1.0, 1.5, 2.0, 1.0}},
        {"coral", {0.05, 0.2, 1.0, 1.0, 1.0, 1.0}},
    };
    for (const auto& [origin, d] : table) {
        const std::string base = std::string("process_multipliers.") + origin + ".";
        IslandParams::ProcessMultipliers m;
        m.fluvial = r.number(base + "fluvial", d.fluvial);
        m.hillslope = r.number(base + "hillslope", d.hillslope);
        m.thermal = r.number(base + "thermal", d.thermal);
        m.marine = r.number(base + "marine", d.marine);
        m.marineDiffusion = r.number(base + "marine_diffusion", d.marineDiffusion);
        m.glacial = r.number(base + "glacial", d.glacial);
        for (double v : {m.fluvial, m.hillslope, m.thermal, m.marine, m.marineDiffusion, m.glacial})
            if (v < 0.0)
                throw ConfigError("'" + base + "*' must be >= 0");
        p.processMultipliers[origin] = m;
    }
}

void resolveClimate(ParamResolver& r, IslandParams::Climate& c) {
    c.baseTemperatureC = r.uniform("climate.base_temperature_c", -10, 30);
    c.lapseRateCPerKm = r.number("climate.lapse_rate_c_per_km", 6.5);
    c.humidity = r.uniform("climate.humidity", 0, 1);
    // Wetter with humidity, and warmer air holds more water.
    c.basePrecipitationMPerYear = r.sample("climate.base_precipitation_m_per_year", [&](double u) {
        const double warmth = std::clamp((c.baseTemperatureC + 10.0) / 40.0, 0.0, 1.0);
        return lerp(0.1, 3.5, c.humidity * (0.6 + 0.4 * warmth)) * lerp(0.75, 1.25, u);
    });
    c.wind.directionDeg = r.uniform("climate.wind.direction_deg", 0, 360);
    c.wind.strength = r.uniform("climate.wind.strength", 0, 1);
    c.wind.variabilityDeg = r.uniform("climate.wind.variability_deg", 5, 60);
    // Swell mostly follows the dominant wind.
    c.swell.directionDeg = r.sample("climate.swell.direction_deg", [&](double u) {
        return std::fmod(c.wind.directionDeg + lerp(-45, 45, u) + 360.0, 360.0);
    });
    c.swell.energy = r.uniform("climate.swell.energy", 0, 1);
    c.orographic.condensationRate = r.uniform("climate.orographic.condensation_rate", 0.2, 1.0);
    c.orographic.rainShadowStrength = r.uniform("climate.orographic.rain_shadow_strength", 0.2, 0.9);
    c.orographic.moistureRechargeOverSea = r.uniform("climate.orographic.moisture_recharge_over_sea", 0.05, 0.5);
    c.orographic.q0 = r.number("climate.orographic.q0", 1.0);
    c.orographic.baseRate = r.uniform("climate.orographic.base_rate", 0.02, 0.1);
    if (c.orographic.q0 <= 0.0 || c.orographic.baseRate <= 0.0 || c.orographic.condensationRate < 0.0 ||
        c.orographic.moistureRechargeOverSea < 0.0)
        throw ConfigError("'climate.orographic': q0 and base_rate must be > 0, the other rates >= 0");
}

void resolveTimeline(ParamResolver& r, IslandParams::Timeline& t, const IslandParams::Climate& climate,
                     const IslandParams::Simulation& sim) {
    const double duration = sim.totalDurationYears;
    // Colder climates see more glaciations.
    const double coldness = std::clamp((20.0 - climate.baseTemperatureC) / 30.0, 0.0, 1.0);
    const int maxGlaciations = static_cast<int>(std::lround(1 + 3 * coldness));
    const int count = r.listSize("timeline.glaciations", "timeline.glaciation_count", 0, maxGlaciations);
    for (int i = 0; i < count; ++i) {
        const std::string list = "timeline.glaciations";
        IslandParams::Glaciation g;
        g.startYears = r.uniform(at(list, i, "start_years"), 0, 0.9 * duration);
        g.endYears = r.sample(at(list, i, "end_years"),
                              [&](double u) { return std::min(duration, g.startYears + lerp(0.02, 0.15, u) * duration); });
        g.temperatureOffsetC = r.uniform(at(list, i, "temperature_offset_c"), -12, -3);
        g.seaLevelDropM = r.uniform(at(list, i, "sea_level_drop_m"), 60, 130);
        t.glaciations.push_back(g);
    }

    if (const nlohmann::json* curve = r.configList("timeline.sea_level_curve")) {
        for (std::size_t i = 0; i < curve->size(); ++i) {
            const auto& p = (*curve)[i];
            if (!p.is_object() || !p.contains("t_years") || !p.contains("level_m") || !p["t_years"].is_number() ||
                !p["level_m"].is_number())
                throw ConfigError("'timeline.sea_level_curve[" + std::to_string(i) +
                                  "]' must be { \"t_years\": <number>, \"level_m\": <number> }");
            t.seaLevelCurve.push_back({p["t_years"].get<double>(), p["level_m"].get<double>()});
        }
        std::sort(t.seaLevelCurve.begin(), t.seaLevelCurve.end(),
                  [](const auto& a, const auto& b) { return a.tYears < b.tYears; });
        r.record("timeline.sea_level_curve", *curve, ParamSource::Config);
        return;
    }

    // Derived from the glaciations: sea level drops to a low stand mid-glaciation.
    t.seaLevelCurve.push_back({0.0, 0.0});
    for (const auto& g : t.glaciations) {
        t.seaLevelCurve.push_back({g.startYears, 0.0});
        t.seaLevelCurve.push_back({(g.startYears + g.endYears) / 2, -g.seaLevelDropM});
        t.seaLevelCurve.push_back({g.endYears, 0.0});
    }
    t.seaLevelCurve.push_back({duration, 0.0});
    std::sort(t.seaLevelCurve.begin(), t.seaLevelCurve.end(),
              [](const auto& a, const auto& b) { return a.tYears < b.tYears; });
    nlohmann::json curve = nlohmann::json::array();
    for (const auto& p : t.seaLevelCurve)
        curve.push_back({{"t_years", p.tYears}, {"level_m", p.levelM}});
    r.record("timeline.sea_level_curve", curve, ParamSource::Seed);
}

void resolveErosion(ParamResolver& r, IslandParams::Erosion& e, const IslandParams::Climate& climate,
                    const IslandParams::World& world) {
    auto& fl = e.fluvial;
    fl.kBase = r.logUniform("erosion.fluvial.k_base", 1e-6, 1e-4);
    fl.m = r.number("erosion.fluvial.m", 0.45);
    fl.n = r.number("erosion.fluvial.n", 1.0);
    fl.depositionG = r.uniform("erosion.fluvial.deposition_g", 0, 2);
    fl.precipitationWeighted = r.flag("erosion.fluvial.precipitation_weighted", true);
    fl.kSedimentMult = r.number("erosion.fluvial.k_sediment_mult", 5.0);
    fl.depositionTolerance = r.number("erosion.fluvial.deposition_tolerance", 1e-3);
    fl.depositionMaxIterations = r.whole("erosion.fluvial.deposition_max_iterations", 20);
    auto& md = e.marineDiffusion;
    // Scaled to these maps (a few km): published values (~100-1000 m2/yr) are
    // for margins a hundred times larger, and D scales with length squared.
    md.sandD0 = r.logUniform("erosion.marine_diffusion.sand_d0", 0.05, 1.0);
    md.finesD0 = r.sample("erosion.marine_diffusion.fines_d0", [&](double u) { return md.sandD0 * lerp(5.0, 20.0, u); });
    md.depthScaleM = r.uniform("erosion.marine_diffusion.depth_scale_m", 15.0, 60.0);
    md.mobileLayerM = r.number("erosion.marine_diffusion.mobile_layer_m", 1.0);
    if (fl.depositionG < 0.0 || fl.depositionTolerance <= 0.0 || fl.depositionMaxIterations < 1 || md.sandD0 < 0.0 ||
        md.finesD0 < 0.0 || md.depthScaleM <= 0.0 || md.mobileLayerM <= 0.0)
        throw ConfigError("'erosion': deposition_g >= 0, deposition_tolerance > 0, deposition_max_iterations >= 1, "
                          "marine_diffusion d0 >= 0 and depth_scale_m > 0");

    e.hillslope.diffusivityM2PerYear = r.logUniform("erosion.hillslope.diffusivity_m2_per_year", 1e-3, 1e-1);

    e.thermal.iterationsPerStep = r.whole("erosion.thermal.iterations_per_step", 1);
    e.thermal.rate = r.number("erosion.thermal.rate", 0.5);
    e.thermal.sedimentTalusAngleDeg = r.number("erosion.thermal.sediment_talus_angle_deg", 33.0);
    if (fl.kSedimentMult < 0.0 || e.thermal.rate < 0.0 || e.thermal.rate > 1.0 ||
        e.thermal.sedimentTalusAngleDeg <= 0.0 || e.thermal.sedimentTalusAngleDeg >= 90.0 ||
        e.thermal.iterationsPerStep < 0 || e.hillslope.diffusivityM2PerYear < 0.0)
        throw ConfigError("'erosion': k_sediment_mult and diffusivity must be >= 0, thermal.rate in [0, 1], "
                          "sediment_talus_angle_deg in ]0, 90[, iterations_per_step >= 0");

    auto& gl = e.glacial;
    gl.accumulationRate = r.uniform("erosion.glacial.accumulation_rate", 0.5, 1.5);
    gl.ablationGradient = r.uniform("erosion.glacial.ablation_gradient", 0.3, 1.5);
    r.markConsumed("erosion.glacial.deformation_coeff"); // replaced by glen_A
    // tau_b ~ 1e5 Pa under 100 m of ice on a 10 % slope: sliding of ~5 to 100 m/yr.
    gl.slidingCoeff = r.logUniform("erosion.glacial.sliding_coeff", 5e-10, 1e-8);
    // ~1 mm/yr under ice sliding at 50 m/yr (observed rates: 0.1 to 10 mm/yr).
    gl.erosionCoeff = r.logUniform("erosion.glacial.erosion_coeff", 5e-6, 5e-5);
    gl.erosionExponent = r.number("erosion.glacial.erosion_exponent", 1.0);
    gl.glenA = r.number("erosion.glacial.glen_A", 2.4e-24);
    gl.iceDensity = r.number("erosion.glacial.ice_density", 917.0);
    gl.snowTemperatureC = r.number("erosion.glacial.snow_temperature_c", 0.0);
    gl.maxSubsteps = r.whole("erosion.glacial.max_substeps", 5000);
    gl.convergenceTol = r.number("erosion.glacial.convergence_tol", 0.01);
    gl.iceThresholdM = r.number("erosion.glacial.ice_threshold_m", 5.0);
    gl.calvingDepthM = r.number("erosion.glacial.calving_depth_m", 30.0);
    gl.iceGridCellM = r.number("erosion.glacial.ice_grid_cell_m", 50.0);
    if (gl.glenA <= 0.0 || gl.iceDensity <= 0.0 || gl.maxSubsteps < 1 || gl.convergenceTol <= 0.0 ||
        gl.iceThresholdM < 0.0 || gl.calvingDepthM < 0.0 || gl.iceGridCellM <= 0.0 || gl.slidingCoeff < 0.0 ||
        gl.erosionCoeff < 0.0 || gl.ablationGradient < 0.0 || gl.accumulationRate < 0.0)
        throw ConfigError("'erosion.glacial': glen_A, ice_density, convergence_tol, ice_grid_cell_m > 0, "
                          "max_substeps >= 1, the other coefficients >= 0");
    auto& reef = e.reef;
    reef.enabled = r.flag("erosion.reef.enabled", true);
    reef.growthRate = r.logUniform("erosion.reef.growth_rate", 2e-3, 1e-2);
    reef.maxDepthM = r.uniform("erosion.reef.max_depth", 10, 30);

    auto& co = e.coastal;
    // m2 of cliff section per year and per meter of coast: a 20 m cliff of
    // hardness 0.5 facing the swell over deep water retreats by ~1 mm to 0.1 m/yr
    // (then slower as its platform widens).
    co.waveErosionCoeff = r.logUniform("erosion.coastal.wave_erosion_coeff", 0.01, 1.0);
    co.fetchRayCount = r.whole("erosion.coastal.fetch_ray_count", 32);
    co.fetchMaxDistanceM = r.uniform("erosion.coastal.fetch_max_distance_m", 2000, 20000);
    co.platformDepthM = r.uniform("erosion.coastal.platform_depth_m", 3, 15);
    co.sedimentExportThreshold = r.uniform("erosion.coastal.sediment_export_threshold", 0.3, 0.9);
    co.exposureUpdateIntervalSteps = r.whole("erosion.coastal.exposure_update_interval_steps", 10);
    co.platformAttenuationM = r.number("erosion.coastal.platform_attenuation_m", 200.0);
    co.sedimentHardness = r.number("erosion.coastal.sediment_hardness", 0.1);
    if (co.waveErosionCoeff < 0.0 || co.platformDepthM < 0.0 || co.exposureUpdateIntervalSteps < 1 ||
        co.sedimentHardness <= 0.0 || co.fetchRayCount < 1 || co.fetchMaxDistanceM <= 0.0)
        throw ConfigError("'erosion.coastal': coefficients >= 0, platform_depth_m >= 0 (below sea level), "
                          "intervals and ray count >= 1, sediment_hardness and fetch > 0");

    auto& dh = e.detailHydraulic;
    dh.method = r.text("erosion.detail_hydraulic.method", "droplet", {"droplet", "pipe"});
    const double area = static_cast<double>(world.width) * world.length;
    dh.dropletCount = r.sampleInteger("erosion.detail_hydraulic.droplet_count",
                                      [&](double u) { return static_cast<int>(std::lround(area * lerp(0.05, 0.3, u))); });
    dh.inertia = r.number("erosion.detail_hydraulic.inertia", 0.05);
    dh.capacity = r.number("erosion.detail_hydraulic.capacity", 4.0);
    dh.erosionRate = r.number("erosion.detail_hydraulic.erosion_rate", 0.3);
    dh.depositionRate = r.number("erosion.detail_hydraulic.deposition_rate", 0.3);
    dh.evaporation = r.number("erosion.detail_hydraulic.evaporation", 0.01);
    dh.radius = r.whole("erosion.detail_hydraulic.radius", 3);
    dh.maxLifetime = r.whole("erosion.detail_hydraulic.max_lifetime", 64);

    auto& ae = e.aeolian;
    // Dunes need dry air and wind.
    ae.enabled = r.chance("erosion.aeolian.enabled",
                          std::clamp(0.6 * (1.0 - climate.humidity) + 0.3 * climate.wind.strength, 0.0, 1.0));
    ae.sandSupply = r.uniform("erosion.aeolian.sand_supply", 0, 1);
    ae.iterations = r.integer("erosion.aeolian.iterations", 50, 400);
    ae.slabHeightM = r.number("erosion.aeolian.slab_height_m", 0.2);
    ae.hopLengthM = r.uniform("erosion.aeolian.hop_length_m", 1, 5);
    ae.depositProbSand = r.number("erosion.aeolian.deposit_prob_sand", 0.6);
    ae.depositProbBare = r.number("erosion.aeolian.deposit_prob_bare", 0.4);
    ae.reposeAngleDeg = r.number("erosion.aeolian.repose_angle_deg", 34);
    ae.shadowAngleDeg = r.number("erosion.aeolian.shadow_angle_deg", 15);
}

void resolveHydrology(ParamResolver& r, IslandParams::Hydrology& h) {
    h.riverMinAreaM2 = r.logUniform("hydrology.river_min_area_m2", 2e4, 5e5);
    h.widthCoeff = r.uniform("hydrology.width_coeff", 0.002, 0.02);
    h.widthExponent = r.number("hydrology.width_exponent", 0.5);
    h.depthCoeff = r.uniform("hydrology.depth_coeff", 0.002, 0.015);
    h.depthExponent = r.number("hydrology.depth_exponent", 0.4);
    h.lakeMinAreaM2 = r.logUniform("hydrology.lake_min_area_m2", 500, 2e4);
    h.waterfallMinDropBlocks = r.whole("hydrology.waterfall_min_drop_blocks", 2);
}

void resolveRest(ParamResolver& r, IslandParams& p) {
    auto& pc = p.presentClimate;
    pc.temperatureOffsetC = r.number("present_climate.temperature_offset_c", 0.0);
    pc.glacierMinThicknessM = r.uniform("present_climate.glacier_min_thickness_m", 5, 30);
    pc.snowMinAltitudeOffsetM = r.number("present_climate.snow_min_altitude_offset_m", 0.0);

    p.soil.formationRate = r.logUniform("soil.formation_rate", 1e-5, 1e-3);
    p.soil.maxDepthM = r.uniform("soil.max_depth_m", 0.5, 3.0);
    p.soil.slopeLimitDeg = r.uniform("soil.slope_limit_deg", 30, 45);

    p.biomes.transitionNoise.frequency = r.logUniform("biomes.transition_noise.frequency", 1, 10);
    p.biomes.transitionNoise.amplitude = r.uniform("biomes.transition_noise.amplitude", 0.05, 0.3);
    p.biomes.table = r.raw("biomes.table");

    auto& v = p.voxelization;
    v.surfaceMaterialBySlope = r.flag("voxelization.surface_material_by_slope", true);
    v.strataJitterM = r.uniform("voxelization.strata_jitter_m", 0, 5);
    v.overhangs.enabled = r.flag("voxelization.overhangs.enabled", true);
    v.overhangs.minSlopeDeg = r.number("voxelization.overhangs.min_slope_deg", 60);
    v.overhangs.noiseFrequency = r.logUniform("voxelization.overhangs.noise_frequency", 25, 125);
    v.overhangs.depthBlocks = r.integer("voxelization.overhangs.depth_blocks", 1, 4);
    v.seaNotches.enabled = r.flag("voxelization.sea_notches.enabled", true);
    v.seaNotches.minExposure = r.uniform("voxelization.sea_notches.min_exposure", 0.4, 0.8);
    v.seaNotches.heightBlocks = r.integer("voxelization.sea_notches.height_blocks", 1, 3);
    v.seaNotches.depthBlocks = r.integer("voxelization.sea_notches.depth_blocks", 1, 4);

    p.caves.enabled = r.flag("caves.enabled", false);
    p.vegetation.enabled = r.flag("vegetation.enabled", false);
    p.structures.enabled = r.flag("structures.enabled", false);

    p.debug.stopAfterStage = r.optionalText("debug.stop_after_stage");
    p.debug.dumpStages = r.textList("debug.dump_stages");
    p.debug.cacheDir = r.text("debug.cache_dir", "./cache");
    p.debug.forceRecomputeFrom = r.optionalText("debug.force_recompute_from");
}

// Top-level order of the resolved JSON (matches island_params.jsonc).
constexpr std::array kSectionOrder{
    "seed",      "world",      "simulation", "origin",          "shape",   "initial_topography",
    "geology",   "tectonics",  "climate",    "timeline",        "erosion", "process_multipliers", "hydrology",
    "present_climate", "soil", "biomes",     "voxelization",    "caves",   "vegetation",
    "structures", "debug",
};

} // namespace

ResolvedParams resolveIslandParams(std::uint64_t seed, const nlohmann::json& config,
                                   const std::vector<std::string>& extraKeys) {
    if (!config.is_object())
        throw ConfigError("config root must be a JSON object");

    ParamResolver r(seed, config);
    r.markConsumed("seed");
    r.record("seed", config.contains("seed") ? config["seed"] : nlohmann::json(seed), ParamSource::Config);
    for (const auto& key : extraKeys)
        r.markConsumed(key);

    // Resolution order follows dependencies, not the file order.
    IslandParams p;
    p.seed = seed;
    resolveWorld(r, p.world);
    resolveOrigin(r, p.origin);
    resolveShape(r, p);
    resolveSimulation(r, p);
    resolveBathymetry(r, p);
    resolveInitialTopography(r, p);
    resolveClimate(r, p.climate);
    resolveErosion(r, p.erosion, p.climate, p.world); // before tectonics: uplift rates follow k_base
    resolveTectonics(r, p); // before geology: intrusions follow the collision, derived faults
    resolveGeology(r, p.geology, p.origin, p.islandRadiusM(), p.shape.mainAxisDeg, p.tectonics);
    resolveTimeline(r, p.timeline, p.climate, p.simulation);
    resolveHydrology(r, p.hydrology);
    resolveProcessMultipliers(r, p);
    resolveRest(r, p);
    r.checkNoUnknownKeys();

    // Display order: by section in file order, resolution order within a section.
    auto entries = r.takeEntries();
    const auto sectionRank = [](const std::string& path) {
        const std::string section = path.substr(0, path.find_first_of(".["));
        const auto it = std::find(kSectionOrder.begin(), kSectionOrder.end(), section);
        return static_cast<int>(it - kSectionOrder.begin());
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const ParamEntry& a, const ParamEntry& b) { return sectionRank(a.path) < sectionRank(b.path); });

    return ResolvedParams{std::move(p), std::move(entries), config, r.takeNotes()};
}

nlohmann::ordered_json ResolvedParams::toJson() const {
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    for (const auto& entry : entries) {
        // Walk/create the path: "a.b[2].c".
        nlohmann::ordered_json* node = &root;
        std::size_t start = 0;
        while (true) {
            const std::size_t dot = entry.path.find('.', start);
            const std::string part = entry.path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            const std::size_t bracket = part.find('[');
            node = &(*node)[part.substr(0, bracket)];
            for (std::size_t b = bracket; b != std::string::npos; b = part.find('[', b + 1)) {
                const std::size_t index = std::stoul(part.substr(b + 1, part.find(']', b) - b - 1));
                if (!node->is_array())
                    *node = nlohmann::ordered_json::array();
                while (node->size() <= index)
                    node->push_back(nlohmann::ordered_json::object());
                node = &(*node)[index];
            }
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        *node = nlohmann::ordered_json(entry.value);
    }
    return root;
}

} // namespace wg
