// Distribution of every island parameter.
//
// Ranges noted [a, b] in island_params.jsonc are used as given; the others are
// plausible values chosen here and can be tuned freely. Some distributions
// depend on already-resolved parameters (mostly origin.type):
// they use the final value, so overriding origin.type also changes what is
// drawn for the parameters that depend on it, while the random streams
// themselves stay the same.

#include "wg/params/IslandParams.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "wg/core/Config.hpp"
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

void resolveSimulation(ParamResolver& r, IslandParams::Simulation& s) {
    s.resolution = r.whole("simulation.resolution", 1024);
    if (s.resolution < 2)
        throw ConfigError("'simulation.resolution' must be >= 2");
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
    // Crater and caldera outlines are not perfect circles.
    v.summitIrregularity = r.uniform("initial_topography.volcano.summit_irregularity", 0.05, 0.2);
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
        v.summitIrregularity < 0.0 || v.summitIrregularity >= 1.0 ||
        v.lavaFlowWidthM <= 0.0 || a.pinnacleRadiusM <= 0.0)
        throw ConfigError("'initial_topography.volcano' and 'initial_topography.atoll' sizes must be positive");
}

// Relief zones by origin: continental islands get 1 to 3 zones, with a range
// about 60 % of the time (the first range follows the main axis); volcanic
// islands at most low hills (the cone makes the relief); sedimentary and
// coral islands none. Sizes are relative to the island radius, heights to
// the zone width (bounded slopes).
void resolveRelief(ParamResolver& r, IslandParams& p) {
    auto& rel = p.relief;
    const std::string& origin = p.origin.type;
    const double R = p.islandRadiusM();
    const double axis = p.shape.mainAxisDeg * std::numbers::pi / 180.0;
    const double elongation = p.shape.elongation;

    const auto [minCount, maxCount] = perOrigin<std::pair<int, int>>(origin, {{{0, 1}, {1, 3}, {0, 0}, {0, 0}}});
    const int count = r.listSize("relief.zones", "relief.zone_count", minCount, maxCount);
    bool rangeSeen = false;
    for (int i = 0; i < count; ++i) {
        const std::string list = "relief.zones";
        IslandParams::ReliefZone z;
        const Weights types = origin != "continental" ? Weights{{"hills", 1.0}}
                              : i == 0               ? Weights{{"range", 0.6}, {"massif", 0.25}, {"hills", 0.15}}
                                                     : Weights{{"range", 0.15}, {"massif", 0.45}, {"hills", 0.4}};
        z.type = r.pick(at(list, i, "type"), types, {"range", "massif", "hills"});
        const bool range = z.type == "range", hills = z.type == "hills";
        // Center in the island frame (u along the main axis, v across it,
        // compressed like the island), then in the world.
        const double reachU = range ? 0.3 : 0.5, reachV = (range ? 0.25 : 0.4) / elongation;
        z.center = r.point(at(list, i, "center"), [&](double a, double b) {
            const double u = R * reachU * (2.0 * a - 1.0), v = R * reachV * (2.0 * b - 1.0);
            return IslandParams::Vec2{u * std::cos(axis) - v * std::sin(axis), u * std::sin(axis) + v * std::cos(axis)};
        });
        const bool mainRange = range && !rangeSeen;
        rangeSeen = rangeSeen || range;
        z.angleDeg = r.sample(at(list, i, "angle_deg"), [&](double u) { return mainRange ? p.shape.mainAxisDeg : 180.0 * u; });
        z.widthM = r.sample(at(list, i, "width_m"), [&](double u) {
            return R * (range ? lerp(0.2, 0.45, u) : hills ? lerp(0.5, 1.0, u) : lerp(0.3, 0.6, u));
        });
        z.lengthM = r.sample(at(list, i, "length_m"), [&](double u) {
            return range ? R * lerp(0.8, 1.6, u) : z.widthM * (hills ? lerp(1.0, 2.0, u) : lerp(1.0, 1.5, u));
        });
        z.heightM = r.sample(at(list, i, "height_m"), [&](double u) {
            if (hills) // volcanic hills are weaker still
                return z.widthM * (origin == "volcanic" ? lerp(0.02, 0.06, u) : lerp(0.04, 0.12, u));
            return z.widthM * (range ? lerp(0.5, 1.1, u) : lerp(0.4, 0.9, u));
        });
        if (z.widthM <= 0.0 || z.lengthM <= 0.0 || z.heightM < 0.0)
            throw ConfigError("'" + at(list, i, "") + "': width and length must be > 0, height >= 0");
        rel.zones.push_back(z);
    }
    rel.warpAmplitude = r.uniform("relief.warp_amplitude", 0.1, 0.3);
    rel.modulation = r.uniform("relief.modulation", 0.0, 0.6);

    auto& n = rel.mountainNoise;
    n.frequency = r.logUniform("relief.mountain_noise.frequency", 1.5, 4.0);
    n.octaves = r.integer("relief.mountain_noise.octaves", 4, 7);
    n.lacunarity = r.uniform("relief.mountain_noise.lacunarity", 1.9, 2.2);
    n.gain = r.uniform("relief.mountain_noise.gain", 0.4, 0.6);
    n.anisotropy = r.uniform("relief.mountain_noise.anisotropy", 2.0, 4.0);
    n.warpAmplitude = r.uniform("relief.mountain_noise.warp_amplitude", 0.1, 0.35);
    if (n.frequency <= 0.0 || n.octaves < 1 || n.anisotropy < 1.0 || rel.warpAmplitude < 0.0 || rel.modulation < 0.0)
        throw ConfigError("'relief': frequency must be > 0, octaves >= 1, anisotropy >= 1, amplitudes >= 0");
}

void resolveGeology(ParamResolver& r, IslandParams::Geology& g, const IslandParams::Origin& origin, double radius,
                    double mainAxisDeg) {
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
    // Continental folds follow the main axis of the island (the orogeny that made them).
    f.axisAngleDeg = r.sample("geology.folding.axis_angle_deg",
                              [&](double u) { return origin.type == "continental" ? mainAxisDeg : 180.0 * u; });
    // Folds a few hundred meters to a couple of kilometers wide, with an
    // amplitude of 3 to 20 % of the wavelength (beyond, limbs get near vertical).
    f.wavelengthM = r.sample("geology.folding.wavelength_m", [&](double u) { return radius * logLerp(0.4, 1.5, u); });
    f.amplitudeM = r.sample("geology.folding.amplitude_m", [&](double u) { return f.wavelengthM * lerp(0.03, 0.2, u); });
    f.asymmetry = r.uniform("geology.folding.asymmetry", 0, 1);
    f.plungeDeg = r.uniform("geology.folding.plunge_deg", 0, 20);
}

// Erosion defaults follow SimpleHydrology; see island_params.jsonc. Presets by
// origin (columns: volcanic, continental, sedimentary, coral): continental
// islands need many steps to carve their mountains; volcanic ones go from
// barely to deeply eroded (the seed decides) and entrain more (radial ravines
// on the flanks); the low sedimentary and coral islands, with almost no relief
// to shape, get only a few steps.
void resolveErosion(ParamResolver& r, IslandParams& p) {
    auto& e = p.erosion.particles;
    const std::string& origin = p.origin.type;
    e.enabled = r.flag("erosion.particles.enabled", true);
    e.steps = r.sampleInteger("erosion.particles.steps", [&](double u) {
        const auto [min, max] = perOrigin<std::pair<int, int>>(origin, {{{2000, 5000}, {4000, 5000}, {100, 100}, {50, 50}}});
        return min + static_cast<int>(std::lround(u * (max - min)));
    });
    e.particlesPerStep = r.whole("erosion.particles.particles_per_step", 0);
    e.maxAge = r.whole("erosion.particles.max_age", 512);
    e.gravity = r.number("erosion.particles.gravity", 1.0);
    e.momentumTransfer = r.number("erosion.particles.momentum_transfer", 1.0);
    e.entrainment = r.number("erosion.particles.entrainment", perOrigin<double>(origin, {{14.0, 10.0, 10.0, 10.0}}));
    e.depositionRate = r.number("erosion.particles.deposition_rate", 0.1);
    e.evaporationRate = r.number("erosion.particles.evaporation_rate", 0.001);
    e.minVolume = r.number("erosion.particles.min_volume", 0.01);
    e.dischargeLrate = r.number("erosion.particles.discharge_lrate", 0.1);
    e.dischargeScale = r.number("erosion.particles.discharge_scale", 0.4);
    e.hardnessInfluence = r.number("erosion.particles.hardness_influence", 1.0);
    // Default: the cell size, i.e. the true geometry (no vertical exaggeration).
    const double cellM = p.world.width * p.world.blockSizeM / p.simulation.resolution;
    e.heightScaleM = r.number("erosion.particles.height_scale_m", cellM);
    e.underwaterSteps = r.whole("erosion.particles.underwater_steps", 10);
    if (e.steps < 0 || e.particlesPerStep < 0 || e.underwaterSteps < 0 || e.maxAge < 1 || e.gravity <= 0.0 || e.depositionRate < 0.0 ||
        e.depositionRate > 1.0 || e.evaporationRate < 0.0 || e.evaporationRate >= 1.0 || e.minVolume <= 0.0 ||
        e.dischargeLrate <= 0.0 || e.dischargeLrate > 1.0 || e.dischargeScale <= 0.0 || e.heightScaleM <= 0.0)
        throw ConfigError("'erosion.particles': steps, particles_per_step, underwater_steps >= 0, max_age >= 1, gravity, min_volume, "
                          "discharge_scale, height_scale_m > 0, deposition_rate and discharge_lrate in (0, 1], "
                          "evaporation_rate in [0, 1)");

    auto& t = p.erosion.thermal;
    t.enabled = r.flag("erosion.thermal.enabled", true);
    t.rate = r.uniform("erosion.thermal.rate", 0.6, 1.0);
    t.sedimentTalusAngleDeg = r.number("erosion.thermal.sediment_talus_angle_deg", 33);
    t.underwaterTalusAngleDeg = r.number("erosion.thermal.underwater_talus_angle_deg", 18);
    if (t.rate < 0.0 || t.rate > 1.0 || t.sedimentTalusAngleDeg <= 0.0 || t.sedimentTalusAngleDeg >= 90.0 ||
        t.underwaterTalusAngleDeg <= 0.0 || t.underwaterTalusAngleDeg >= 90.0)
        throw ConfigError("'erosion.thermal': rate in [0, 1], talus angles in (0, 90)");
}

void resolveHydrology(ParamResolver& r, IslandParams::Hydrology& h) {
    h.enabled = r.flag("hydrology.enabled", true);
    h.riverMinAreaM2 = r.number("hydrology.river_min_area_m2", 15000.0);
    h.riverWidthCoeff = r.number("hydrology.river_width_coeff", 0.01);
    h.riverMinWidthM = r.number("hydrology.river_min_width_m", 1.0);
    h.riverDepthRatio = r.number("hydrology.river_depth_ratio", 0.4);
    h.riverBankSlopeDeg = r.number("hydrology.river_bank_slope_deg", 35.0);
    h.lakeMinDepthM = r.number("hydrology.lake_min_depth_m", 1.0);
    h.lakeMinAreaM2 = r.number("hydrology.lake_min_area_m2", 1000.0);
    h.breachMaxDepthM = r.number("hydrology.breach_max_depth_m", 3.0);
    h.breachMaxLengthM = r.number("hydrology.breach_max_length_m", 300.0);
    if (h.riverMinAreaM2 <= 0.0 || h.riverWidthCoeff < 0.0 || h.riverMinWidthM < 0.0 || h.riverDepthRatio < 0.0 ||
        h.lakeMinDepthM < 0.0 || h.lakeMinAreaM2 < 0.0 || h.breachMaxDepthM < 0.0 || h.breachMaxLengthM < 0.0)
        throw ConfigError("'hydrology': river_min_area_m2 > 0, the other sizes >= 0");
    if (h.riverBankSlopeDeg <= 0.0 || h.riverBankSlopeDeg >= 90.0)
        throw ConfigError("'hydrology.river_bank_slope_deg' must be in ]0, 90[");
}

void resolveVoxelization(ParamResolver& r, IslandParams::Voxelization& v) {
    v.enabled = r.flag("voxelization.enabled", true);
    v.dirtDepthBlocks = r.whole("voxelization.dirt_depth_blocks", 3);
    v.grassMaxSlopeDeg = r.number("voxelization.grass_max_slope_deg", 40.0);
    if (v.dirtDepthBlocks < 0 || v.grassMaxSlopeDeg < 0.0 || v.grassMaxSlopeDeg > 90.0)
        throw ConfigError("'voxelization': dirt_depth_blocks >= 0, grass_max_slope_deg in [0, 90]");
}

// Not used yet: will weight the rain and drive the dunes.
void resolveClimate(ParamResolver& r, IslandParams::Climate& c) {
    c.humidity = r.uniform("climate.humidity", 0, 1);
    c.wind.directionDeg = r.uniform("climate.wind.direction_deg", 0, 360);
    c.wind.strength = r.uniform("climate.wind.strength", 0, 1);
    c.wind.variabilityDeg = r.uniform("climate.wind.variability_deg", 5, 60);
}

// Top-level order of the resolved JSON (matches island_params.jsonc).
constexpr std::array kSectionOrder{
    "seed",    "world",   "simulation",   "origin",  "shape", "initial_topography", "relief", "geology",
    "erosion", "hydrology", "voxelization", "climate",
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
    resolveWorld(r, p.world);
    resolveOrigin(r, p.origin);
    resolveShape(r, p);
    resolveSimulation(r, p.simulation);
    resolveBathymetry(r, p);
    resolveInitialTopography(r, p);
    resolveRelief(r, p);
    resolveGeology(r, p.geology, p.origin, p.islandRadiusM(), p.shape.mainAxisDeg);
    resolveErosion(r, p);
    resolveHydrology(r, p.hydrology);
    resolveVoxelization(r, p.voxelization);
    resolveClimate(r, p.climate);
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

    return ResolvedParams{std::move(p), std::move(entries), config};
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
