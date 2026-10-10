// Minimal self-contained tests (no framework yet). Run: ctest --test-dir build

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <filesystem>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/gen/StageRegistry.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/island/Geology.hpp"
#include "wg/island/IslandGrid.hpp"
#include "wg/island/IslandLayers.hpp"
#include "wg/render/MapRenderer.hpp"
#include "wg/params/IslandParams.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

template <typename Error, typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const Error&) {
        return true;
    }
    return false;
}

// --- Test stages -----------------------------------------------------------------

// Sets a world-sized layer to a constant.
class FillStage final : public wg::Stage {
public:
    explicit FillStage(wg::StageParams& params)
        : layer_(params.get<std::string>("layer", "height")), value_(params.get("value", 0.0f)) {}

    void run(wg::WorldState& world, const wg::StageContext&) override { world.createLayer(layer_, value_); }

private:
    std::string layer_;
    float value_;
};

// Adds `amount` to a layer, one row at a time, reporting progress after each row.
class AddStage final : public wg::Stage {
public:
    explicit AddStage(wg::StageParams& params)
        : layer_(params.get<std::string>("layer", "height")), amount_(params.get("amount", 1.0f)) {}

    void run(wg::WorldState& world, const wg::StageContext& context) override {
        auto& grid = world.editLayer(layer_); // kept across progress() calls on purpose
        for (int z = 0; z < grid.length(); ++z) {
            for (int x = 0; x < grid.width(); ++x)
                grid.at(x, z) += amount_;
            context.progress(static_cast<float>(z + 1) / grid.length());
        }
    }

private:
    std::string layer_;
    float amount_;
};

// Builds a voxel volume with a column of stone up to y = 10.
class StoneStage final : public wg::Stage {
public:
    explicit StoneStage(wg::StageParams&) {}
    void run(wg::WorldState& world, const wg::StageContext&) override {
        world.createVoxels().fillBox(0, world.info().minY, 0, world.info().width, 11, world.info().length,
                                     wg::Block::Stone);
    }
};

const wg::StageRegistry& testRegistry() {
    static const wg::StageRegistry registry = [] {
        wg::StageRegistry r;
        r.add<FillStage>("flat");
        for (const std::string type : {"parameters", "island_init"})
            r.add(type, [type](wg::StageParams& p) { return wg::StageRegistry::builtin().create(type, p); });
        r.add<AddStage>("add");
        r.add<StoneStage>("stone");
        return r;
    }();
    return registry;
}

wg::WorldConfig makeConfig(nlohmann::json pipeline, int width = 16, int length = 16) {
    return wg::WorldConfig::fromJson(
        {{"seed", 1}, {"world", {{"width", width}, {"length", length}}}, {"pipeline", pipeline}});
}


wg::Pipeline makePipeline(const wg::WorldConfig& config) {
    return wg::Pipeline::fromConfig(config, testRegistry());
}

// --- Tests -------------------------------------------------------------------------

void testConfig() {
    const auto config = wg::WorldConfig::fromJson({{"seed", 42}, {"world", {{"width", 64}, {"length", 32}}}});
    CHECK(config.seed == 42 && config.info().width == 64 && config.info().length == 32);

    CHECK(wg::WorldConfig::fromJson({{"seed", "abc"}}).seed == wg::hashString("abc"));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"world", {{"width", 1}}}}); }));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"seed", 1}, {"world", {{"width", 0}}}}); }));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"seed", 1}, {"world", {{"width", 1.5}}}}); }));
}

// --- Parameters ------------------------------------------------------------------

const wg::ParamEntry* entry(const wg::ResolvedParams& params, const std::string& path) {
    for (const auto& e : params.entries)
        if (e.path == path)
            return &e;
    return nullptr;
}

wg::ResolvedParams resolve(const nlohmann::json& config) {
    return *wg::WorldConfig::fromJson(config).params;
}

void testParamsDeterminism() {
    const auto a = resolve({{"seed", 7}});
    const auto b = resolve({{"seed", 7}});
    const auto c = resolve({{"seed", 8}});
    CHECK(a.toJson() == b.toJson());
    CHECK(a.toJson() != c.toJson());
    CHECK(entry(a, "shape.radius_ratio")->source == wg::ParamSource::Seed);
    CHECK(entry(a, "world.width")->source == wg::ParamSource::Default);
}

void testParamsOverrideDoesNotShiftOthers() {
    const auto base = resolve({{"seed", 7}});
    const auto overridden = resolve({{"seed", 7}, {"shape", {{"radius_ratio", 0.5}, {"lobe_count", nullptr}}},
                                     {"climate", {{"humidity", 0.9}}}});
    CHECK(overridden.values.shape.radiusRatio == 0.5);
    CHECK(entry(overridden, "shape.radius_ratio")->source == wg::ParamSource::Config);
    CHECK(entry(overridden, "shape.lobe_count")->source == wg::ParamSource::Seed); // null = drawn
    // Unrelated parameters keep their value.
    CHECK(overridden.values.shape.lobeCount == base.values.shape.lobeCount);
    CHECK(overridden.values.shape.lobeSpread == base.values.shape.lobeSpread);
    CHECK(overridden.values.simulation.dtYears == base.values.simulation.dtYears);
    CHECK(overridden.values.erosion.fluvial.kBase == base.values.erosion.fluvial.kBase);
    // Dependent parameters follow the override (precipitation depends on humidity).
    CHECK(overridden.values.climate.basePrecipitationMPerYear != base.values.climate.basePrecipitationMPerYear);
}

void testParamsDependencies() {
    const auto coral = resolve({{"seed", 3}, {"origin", {{"type", "coral"}}}});
    const auto volcanic = resolve({{"seed", 3}, {"origin", {{"type", "volcanic"}}}});
    // Same random streams, so the same number of strata, but materials suited to the origin.
    CHECK(coral.values.geology.strata.size() == volcanic.values.geology.strata.size());
    for (const auto& s : coral.values.geology.strata)
        CHECK(s.material == "limestone" || s.material == "sandstone");
    for (const auto& s : volcanic.values.geology.strata)
        CHECK(s.material == "basalt" || s.material == "tuff" || s.material == "limestone");
    CHECK(volcanic.values.geology.basementMaterial == "basalt");
}

void testParamsLists() {
    const auto base = resolve({{"seed", 5}});
    // A list given in the config sets the count; missing fields are drawn.
    const auto partial = resolve({{"seed", 5},
                                  {"geology", {{"strata", {{{"material", "granite"}}, {{"thickness_m", 12.5}}}}}}});
    CHECK(partial.values.geology.strata.size() == 2);
    CHECK(entry(partial, "geology.strata_count")->source == wg::ParamSource::Config);
    CHECK(partial.values.geology.strata[0].material == "granite");
    CHECK(partial.values.geology.strata[0].thicknessM == base.values.geology.strata[0].thicknessM);
    CHECK(partial.values.geology.strata[1].thicknessM == 12.5);

    const auto counted = resolve({{"seed", 5}, {"geology", {{"fault_count", 3}}}});
    CHECK(entry(counted, "geology.fault_count")->value == 3); // plus faults derived from the tectonic phases

    CHECK(throws<wg::ConfigError>([] {
        resolve({{"seed", 5}, {"geology", {{"strata_count", 4}, {"strata", {{{"material", "granite"}}}}}}});
    }));
    CHECK(throws<wg::ConfigError>([] { resolve({{"seed", 5}, {"geology", {{"strata", {{{"material", "cheese"}}}}}}}); }));
    // A custom material can be used once added to the table.
    const auto custom = resolve({{"seed", 5},
                                 {"geology",
                                  {{"materials", {{"marble", {{"hardness", 0.7}, {"erodibility", 0.8},
                                                              {"talus_angle_deg", 50}, {"sediment", "sand"}}}}},
                                   {"basement_material", "marble"}}}});
    CHECK(custom.values.geology.basementMaterial == "marble" && custom.values.geology.materials.size() == 9);
}

void testParamsValidation() {
    CHECK(throws<wg::ConfigError>([] { resolve({{"seed", 1}, {"shape", {{"radius_ratoi", 0.5}}}}); })); // typo
    CHECK(throws<wg::ConfigError>([] { resolve({{"seed", 1}, {"shape", {{"lobe_count", 2.5}}}}); }));
    CHECK(throws<wg::ConfigError>([] { resolve({{"seed", 1}, {"origin", {{"type", "lunar"}}}}); }));
    CHECK(throws<wg::ConfigError>([] { resolve({{"seed", 1}, {"climate", {{"humidity", "wet"}}}}); }));
}

void testParamsRoundTrip() {
    // The resolved JSON, fed back as a config, reproduces the same parameters.
    const auto original = resolve({{"seed", 11}});
    const auto replayed = resolve(nlohmann::json::parse(original.toJson().dump()));
    CHECK(replayed.toJson() == original.toJson());
    for (const auto& e : replayed.entries) // only null values (e.g. biomes.table) stay defaults
        CHECK(e.source == wg::ParamSource::Config || e.value.is_null());
}

void testIslandParamsFile() {
    const auto config = wg::WorldConfig::load(WG_SOURCE_DIR "/island_params.jsonc");
    CHECK(config.seed == config.raw["seed"].get<std::uint64_t>() && config.info().width == 4096);
    CHECK(config.island().erosion.fluvial.m == 0.45);
    CHECK(config.island().geology.materials.at("shale").erodibility == 2.0);
}

void testPipelineConfig() {
    using J = nlohmann::json;
    const auto build = [](J pipeline) { makePipeline(makeConfig(pipeline)); };
    CHECK(!throws<wg::ConfigError>([&] { build(J::array()); }));
    CHECK(!throws<wg::ConfigError>([&] { build(J::array({{{"type", "flat"}}, {{"type", "add"}}})); }));
    // No "pipeline": the default island pipeline.
    CHECK(wg::Pipeline::fromConfig(wg::WorldConfig::fromJson({{"seed", 1}})).stages().front().name == "parameters");
    CHECK(throws<wg::ConfigError>([&] { build(J::array({{{"type", "nope"}}})); }));
    CHECK(throws<wg::ConfigError>([&] { build(J::array({{{"type", "flat"}, {"vlaue", 3}}})); })); // typo
    CHECK(throws<wg::ConfigError>([&] { build(J::array({{{"type", "flat"}}, {{"type", "flat"}}})); }));
    CHECK(throws<wg::ConfigError>([&] { build(J::array({{{"type", "add"}, {"amount", "4"}}})); }));
}

void testSnapshots() {
    const auto config = makeConfig(nlohmann::json::array({
        {{"type", "flat"}, {"value", 10}},
        {{"type", "add"}, {"amount", 5}},
        {{"type", "flat"}, {"name", "other"}, {"layer", "moisture"}, {"value", 0.5}},
        {{"type", "stone"}},
    }));
    const auto snapshots = makePipeline(config).run(config);
    CHECK(snapshots.size() == 4);

    // Earlier snapshots are not affected by later stages (copy-on-write).
    CHECK(snapshots[0].state.layer("height").at(3, 3) == 10.0f);
    CHECK(snapshots[1].state.layer("height").at(3, 3) == 15.0f);
    CHECK(!snapshots[2].state.hasVoxels() && snapshots[3].state.hasVoxels());
    // Unchanged data is shared, not copied.
    CHECK(snapshots[2].state.layerIdentity("height") == snapshots[1].state.layerIdentity("height"));

    CHECK(snapshots[1].changedLayers == std::vector<std::string>{"height"});
    CHECK(snapshots[2].changedLayers == std::vector<std::string>{"moisture"});
    CHECK(snapshots[3].changedLayers.empty() && snapshots[3].voxelsChanged);
}

void testMissingLayerFails() {
    const auto config = makeConfig(nlohmann::json::array({{{"type", "add"}}}));
    CHECK(throws<wg::GenerationError>([&] { makePipeline(config).run(config); }));
}

void testLiveCallbacks() {
    const auto config = makeConfig(nlohmann::json::array({{{"type", "flat"}, {"value", 0}}, {{"type", "add"}}}));
    const auto pipeline = makePipeline(config);

    std::vector<std::size_t> started, done;
    std::vector<wg::Snapshot> previews;
    std::vector<float> progress;
    wg::PipelineCallbacks callbacks;
    callbacks.previewInterval = 0.0; // every report
    callbacks.onStageStart = [&](std::size_t i) { started.push_back(i); };
    callbacks.onStageDone = [&](std::size_t i, const wg::Snapshot&) { done.push_back(i); };
    callbacks.onStageProgress = [&](std::size_t, wg::Snapshot preview, float p) {
        previews.push_back(std::move(preview));
        progress.push_back(p);
    };
    pipeline.run(config, callbacks);

    CHECK((started == std::vector<std::size_t>{0, 1}));
    CHECK((done == std::vector<std::size_t>{0, 1}));
    CHECK(previews.size() == 16 && progress.back() == 1.0f);
    CHECK(previews.front().changedLayers == std::vector<std::string>{"height"});
    // Previews are frozen even though the stage kept writing through its reference.
    CHECK(previews[0].state.layer("height").at(0, 0) == 1.0f && previews[0].state.layer("height").at(0, 1) == 0.0f);
    CHECK(previews[7].state.layer("height").at(0, 7) == 1.0f && previews[7].state.layer("height").at(0, 8) == 0.0f);

    // Cancellation: between stages, and from inside a stage via progress().
    int reports = 0;
    wg::PipelineCallbacks cancelling;
    cancelling.shouldCancel = [&] { return ++reports > 5; };
    CHECK(throws<wg::GenerationCancelled>([&] { pipeline.run(config, cancelling); }));
    CHECK(reports == 6);
}

// --- Island initialization ---------------------------------------------------------

std::vector<wg::Snapshot> runIsland(nlohmann::json config) {
    const auto loaded = wg::WorldConfig::fromJson(config);
    return wg::Pipeline::fromConfig(loaded).run(loaded); // default pipeline
}

void testIslandInit() {
    const nlohmann::json config = {{"seed", 9},
                                   {"world", {{"width", 512}, {"length", 512}}},
                                   {"simulation", {{"coarse_resolution", 128}}},
                                   {"tectonics", {{"phases", {{{"type", "dome"}}, {{"type", "collision"}}}}}}};
    const auto snapshots = runIsland(config);
    CHECK(snapshots.size() == 2 && snapshots[1].stageName == "island_init");
    const auto& state = snapshots[1].state;
    const auto& params = state.params().values;
    const auto& h = state.layer(wg::layers::kHeight);
    const auto& flags = state.byteLayer(wg::layers::kFlags);
    CHECK(h.width() == 128 && h.length() == 128);

    // Finite heights; FIXED border at the boundary depth.
    bool finite = true, border = true, mask01 = true;
    for (int j = 0; j < h.length(); ++j) {
        for (int i = 0; i < h.width(); ++i) {
            finite = finite && std::isfinite(h.at(i, j));
            const bool edge = i == 0 || j == 0 || i == h.width() - 1 || j == h.length() - 1;
            if (edge)
                border = border && (flags.at(i, j) & wg::layers::kFixed) &&
                         h.at(i, j) == static_cast<float>(params.simulation.boundaryDepthM);
            else
                border = border && flags.at(i, j) == 0;
            const float m = state.layer(wg::layers::kMask).at(i, j);
            mask01 = mask01 && m >= 0.0f && m <= 1.0f;
        }
    }
    CHECK(finite && border && mask01);
    // An island: land in the middle, sea at the edges.
    CHECK(state.layer(wg::layers::kMask).at(64, 64) > 0.5f || state.layer(wg::layers::kCoastDist).at(64, 64) > -200.0f);
    CHECK(state.layer(wg::layers::kMask).at(2, 2) == 0.0f);
    CHECK(state.layer(wg::layers::kHeightInitial).data()[100] == h.data()[100]);

    // Uplift patterns: one per phase, in [-1, 1], 0 on the border.
    for (int k = 0; k < 2; ++k) {
        const auto [min, max] = wg::valueRange(state.layer(wg::layers::upliftPattern(k)));
        CHECK(min >= -1.0f && max <= 1.0f && max > 0.0f);
        CHECK(state.layer(wg::layers::upliftPattern(k)).at(0, 50) == 0.0f);
    }

    // Bit-for-bit deterministic.
    const auto again = runIsland(config);
    for (const auto& name : state.layerNames()) {
        const auto a = state.layer(name).data(), b = again[1].state.layer(name).data();
        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
    }
}

void testIslandFitsInMap() {
    // Whatever the seed: sea along the edges, reaching the boundary depth
    // smoothly (no cliff next to the FIXED ring), and a plausible slope.
    for (int seed = 1; seed <= 20; ++seed) {
        const auto snapshots = runIsland({{"seed", seed},
                                          {"world", {{"width", 1024}, {"length", 1024}}},
                                          {"simulation", {{"coarse_resolution", 128}}}});
        const auto& state = snapshots[1].state;
        const auto& p = state.params().values;
        const auto& h = state.layer(wg::layers::kHeight);
        const auto& mask = state.layer(wg::layers::kMask);
        const double drop = p.shape.shelfDepthM - p.simulation.boundaryDepthM;
        bool sea = true, smooth = true;
        for (int k = 0; k < 128; ++k) {
            for (const auto [i, j] : {std::pair{k, 1}, std::pair{k, 126}, std::pair{1, k}, std::pair{126, k}}) {
                sea = sea && mask.at(i, j) == 0.0f && h.at(i, j) < 0.0f;
                smooth = smooth && std::abs(h.at(i, j) - p.simulation.boundaryDepthM) < 0.1 * drop + 1.0;
            }
        }
        CHECK(sea && smooth);
        CHECK(p.shape.talusSlopeDeg >= 3.0 && p.shape.talusSlopeDeg <= 50.0);
        if (!(sea && smooth))
            std::fprintf(stderr, "  seed %d sea %d smooth %d  ring h %.1f boundary %.1f drop %.1f mask %.3f shelf %.0f talus %.0f\n", seed, sea, smooth,
                         h.at(64, 1), p.simulation.boundaryDepthM, drop, mask.at(64, 1), p.shape.shelfWidthM, p.shape.talusWidthM);
    }
}

// Ratio of the land extent along the main axis to the extent across it.
double landAspect(const wg::WorldState& state) {
    const auto& p = state.params().values;
    const auto& mask = state.layer(wg::layers::kMask);
    const wg::IslandGrid grid(p);
    const double a = p.shape.mainAxisDeg * std::numbers::pi / 180.0;
    double uMin = 1e9, uMax = -1e9, vMin = 1e9, vMax = -1e9;
    for (int j = 0; j < grid.ny; ++j) {
        for (int i = 0; i < grid.nx; ++i) {
            if (mask.at(i, j) < 0.5f)
                continue;
            const double u = grid.x(i) * std::cos(a) + grid.y(j) * std::sin(a);
            const double v = -grid.x(i) * std::sin(a) + grid.y(j) * std::cos(a);
            uMin = std::min(uMin, u), uMax = std::max(uMax, u), vMin = std::min(vMin, v), vMax = std::max(vMax, v);
        }
    }
    return (uMax - uMin) / std::max(1.0, vMax - vMin);
}

void testShapeByOrigin() {
    const auto island = [](int seed, const char* origin) {
        return runIsland({{"seed", seed},
                          {"world", {{"width", 1024}, {"length", 1024}}},
                          {"simulation", {{"coarse_resolution", 128}}},
                          {"origin", {{"type", origin}}}})[1]
            .state;
    };
    for (int seed = 1; seed <= 6; ++seed) {
        const auto sedimentary = island(seed, "sedimentary");
        const auto volcanic = island(seed, "volcanic");
        CHECK(sedimentary.params().values.shape.elongation >= 3.0);
        CHECK(landAspect(sedimentary) > 2.0);    // long and thin
        CHECK(landAspect(volcanic) < 2.0);       // roughly round
        CHECK(volcanic.params().values.shape.lobeCount <= 2);
    }
}

// Whether open water (mask < 0.5) connects a lagoon cell to the map edge.
bool lagoonOpensToSea(const wg::WorldState& state) {
    const auto& mask = state.layer(wg::layers::kMask);
    const auto& lagoon = state.layer(wg::layers::kLagoon);
    const int w = mask.width(), l = mask.length();
    std::vector<char> seen(static_cast<std::size_t>(w) * l, 0);
    std::vector<std::pair<int, int>> stack{{0, 0}};
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        if (i < 0 || j < 0 || i >= w || j >= l || seen[j * w + i] || mask.at(i, j) >= 0.5f)
            continue;
        seen[j * w + i] = 1;
        if (lagoon.at(i, j) > 0.5f)
            return true;
        stack.insert(stack.end(), {{i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1}});
    }
    return false;
}

void testAtoll() {
    const auto atoll = [](int seed, double passFraction) {
        return runIsland({{"seed", seed},
                          {"world", {{"width", 2048}, {"length", 2048}}},
                          {"simulation", {{"coarse_resolution", 256}}},
                          {"origin", {{"type", "coral"}}},
                          {"shape", {{"ring_pass_fraction", passFraction}}}})[1]
            .state;
    };
    for (int seed = 1; seed <= 4; ++seed) {
        const auto closed = atoll(seed, 0.0);
        CHECK(closed.params().values.shape.mode == "ring");
        int lagoonCells = 0, flagged = 0;
        const auto& lagoon = closed.layer(wg::layers::kLagoon);
        const auto& flags = closed.byteLayer(wg::layers::kFlags);
        for (std::size_t k = 0; k < lagoon.size(); ++k) {
            lagoonCells += lagoon.data()[k] > 0.5f;
            flagged += (flags.data()[k] & wg::layers::kInLagoon) != 0;
        }
        CHECK(lagoonCells > 100 && lagoonCells == flagged);
        CHECK(!lagoonOpensToSea(closed)); // a closed ring
        CHECK(lagoonOpensToSea(atoll(seed, 0.3))); // passes
    }
}

// Geology without folds, faults or intrusions: only the strata.
const nlohmann::json kPlainGeology = {{"folding", {{"enabled", false}}}, {"fault_count", 0}, {"intrusion_count", 0}};

wg::WorldState islandOf(int seed, const char* origin, nlohmann::json topography = nlohmann::json::object(),
                        nlohmann::json geology = nlohmann::json::object()) {
    return runIsland({{"seed", seed},
                      {"world", {{"width", 2048}, {"length", 2048}}},
                      {"simulation", {{"coarse_resolution", 256}}},
                      {"origin", {{"type", origin}}},
                      {"initial_topography", topography},
                      {"geology", geology}})[1]
        .state;
}

void testTopographyByOrigin() {
    for (int seed = 1; seed <= 4; ++seed) {
        // Volcanic: a cone close to its full height (minus the summit hollow).
        const auto volcanic = islandOf(seed, "volcanic");
        const auto& vp = volcanic.params().values.initialTopography;
        const auto [vMin, vMax] = wg::valueRange(volcanic.layer(wg::layers::kHeight));
        CHECK(vp.mode == "volcanic_cone");
        // (volcanoes are scaled down to fit the island around their vent)
        CHECK(vMax > 0.2 * vp.volcano.heightM &&
              vMax < vp.volcano.heightM + vp.volcano.lavaFlowHeightM + vp.volcano.flankNoiseM + 1.0);
        // No coastal cliff: near the coast, the cone has come down to its foot.
        const auto& coast = volcanic.layer(wg::layers::kCoastDist);
        const auto& vh = volcanic.layer(wg::layers::kHeight);
        const double band = volcanic.params().values.shape.coastFalloffM / 4.0; // right at the coast
        float coastalMax = 0.0f;
        for (std::size_t k = 0; k < vh.size(); ++k)
            if (coast.data()[k] > 0.0f && coast.data()[k] < band)
                coastalMax = std::max(coastalMax, vh.data()[k]);
        CHECK(coastalMax < 0.5f * vMax + vp.baseElevationM + vp.volcano.flankNoiseM + vp.volcano.lavaFlowHeightM);
        // Strata parallel to the surface: the top layer everywhere (without
        // intrusions, which may outcrop).
        const auto plain = islandOf(seed, "volcanic", nlohmann::json::object(), kPlainGeology);
        const auto& material = plain.layer(wg::layers::kSurfaceMaterial);
        CHECK(std::all_of(material.data().begin(), material.data().end(),
                          [&](float id) { return id == material.data()[0]; }));

        // Atoll: low ring, lagoon between its floor and just below the surface.
        const auto coral = islandOf(seed, "coral", {{"atoll", {{"makatea", false}}}});
        const auto& a = coral.params().values.initialTopography.atoll;
        const auto& h = coral.layer(wg::layers::kHeight);
        const auto& lagoon = coral.layer(wg::layers::kLagoon);
        float landMax = 0.0f, lagoonMin = 0.0f, lagoonMax = -1e9f;
        for (std::size_t k = 0; k < h.size(); ++k) {
            landMax = std::max(landMax, h.data()[k]);
            if (lagoon.data()[k] > 0.5f) {
                lagoonMin = std::min(lagoonMin, h.data()[k]);
                lagoonMax = std::max(lagoonMax, h.data()[k]);
            }
        }
        CHECK(landMax <= a.ringHeightM + a.ringNoiseM + 0.01);
        // (lagoon cells touching the ring are the shore: sea level + the 1 mm jitter)
        CHECK(lagoonMin >= -(a.lagoonDepthM + a.lagoonNoiseM) - 0.01 && lagoonMax < 1e-3f);

        // Barrier: low, below base + crest + noise.
        const auto barrier = islandOf(seed, "sedimentary");
        const auto& t = barrier.params().values.initialTopography;
        const auto [bMin, bMax] = wg::valueRange(barrier.layer(wg::layers::kHeight));
        CHECK(t.mode == "barrier" && bMax > 0.0f);
        CHECK(bMax <= t.baseElevationM + t.barrier.crestHeightM + t.noise.amplitudeM + 0.01);
    }
}

void testUpliftPatterns() {
    using J = nlohmann::json;
    const J phase = {{"center", {0, 0}}, {"angle_deg", 0}, {"length_m", 300}, {"width_m", 100}, {"falloff", 0.1}};
    J phases = J::array();
    for (const char* type : {"block_uplift", "horst_graben", "tilt", "subsidence"}) {
        J p = phase;
        p["type"] = type;
        phases.push_back(p);
    }
    // A big round flat island so that the mask is 1 around the center.
    const auto state = runIsland({{"seed", 2},
                                  {"world", {{"width", 1024}, {"length", 1024}}},
                                  {"simulation", {{"coarse_resolution", 256}}},
                                  {"origin", {{"type", "volcanic"}}},
                                  {"shape", {{"lobe_count", 1}, {"lobe_spread", 0.0}, {"radius_ratio", 0.8}}},
                                  {"tectonics", {{"phases", phases}}}})[1]
                           .state;
    const wg::IslandGrid grid(state.params().values);
    // Pattern value at (x, y) meters from the center (axis = x).
    const auto at = [&](int k, double x, double y) {
        const int i = static_cast<int>(std::floor(x / grid.cellM + grid.nx * 0.5));
        const int j = static_cast<int>(std::floor(y / grid.cellM + grid.ny * 0.5));
        return state.layer(wg::layers::upliftPattern(k)).at(i, j);
    };
    CHECK(at(0, 0, 0) > 0.95f && at(0, 0, 80) < 0.05f && at(0, 200, 0) < 0.05f); // block
    CHECK(at(1, 0, 50) > 0.95f && at(1, 0, 150) < -0.95f && at(1, 0, -50) < -0.95f); // horst / graben
    CHECK(at(2, 0, 50) > 0.45f && at(2, 0, 50) < 0.55f && at(2, 0, -200) < -0.95f); // tilt
    CHECK(at(3, 0, 0) < -0.95f);                                                   // subsidence

    // Derived faults: wherever a fault borders a non-zero pattern, its positive
    // side (left of the trace) is the raised one.
    const auto& p = state.params().values;
    int checked = 0;
    for (const auto& fault : p.geology.faults) {
        const double a = fault.angleDeg * std::numbers::pi / 180.0;
        const double nx = -std::sin(a), ny = std::cos(a), d = 2.0 * grid.cellM;
        if (std::hypot(fault.x, fault.y) > 300.0)
            continue; // outside the island core
        for (int k : {0, 1}) {
            const float raised = at(k, fault.x + nx * d, fault.y + ny * d);
            const float lowered = at(k, fault.x - nx * d, fault.y - ny * d);
            if (raised != lowered) {
                CHECK(raised > lowered);
                ++checked;
            }
        }
    }
    CHECK(checked >= 6); // the 4 block edges and the central horst boundaries
}

void testTectonicsByOrigin() {
    for (int seed = 1; seed <= 12; ++seed) {
        for (const char* origin : {"volcanic", "sedimentary", "coral"}) {
            const auto t = wg::WorldConfig::fromJson({{"seed", seed}, {"origin", {{"type", origin}}}}).island().tectonics;
            CHECK(t.phases.size() <= 1);
            for (const auto& phase : t.phases)
                CHECK(phase.type == (std::string(origin) == "volcanic" ? "dome" : "subsidence"));
        }
    }
}

void testSharedAxis() {
    using J = nlohmann::json;
    const J phases = {{"phases", J::array({{{"type", "dome"}}, {{"type", "collision"}}, {{"type", "collision"}}})}};
    const auto p = wg::WorldConfig::fromJson({{"seed", 4}, {"origin", {{"type", "continental"}}}, {"tectonics", phases}})
                       .island();
    CHECK(p.tectonics.phases[1].angleDeg == p.shape.mainAxisDeg); // the first collision
    CHECK(p.tectonics.phases[2].angleDeg != p.shape.mainAxisDeg);
    CHECK(p.geology.folding.axisAngleDeg == p.shape.mainAxisDeg);

    // Forcing one of them leaves the others alone.
    const auto forced = wg::WorldConfig::fromJson({{"seed", 4},
                                                   {"origin", {{"type", "continental"}}},
                                                   {"tectonics", phases},
                                                   {"geology", {{"folding", {{"axis_angle_deg", 12.0}}}}}})
                            .island();
    CHECK(forced.geology.folding.axisAngleDeg == 12.0);
    CHECK(forced.shape.mainAxisDeg == p.shape.mainAxisDeg && forced.tectonics.phases[1].angleDeg == p.shape.mainAxisDeg);
}

void testGeology() {
    auto params = wg::WorldConfig::fromJson(
                      {{"seed", 1},
                       {"geology", {{"strata", {{{"material", "limestone"}, {"thickness_m", 10}},
                                                {{"material", "shale"}, {"thickness_m", 20}}}},
                                    {"basement_material", "granite"},
                                    {"folding", {{"enabled", false}}},
                                    {"fault_count", 0},
                                    {"intrusion_count", 0}}},
                       {"tectonics", {{"phase_count", 0}}}})
                      .island();
    const wg::Geology geology(params);
    const auto name = [&](int id) { return geology.materialNames()[id]; };
    CHECK(name(geology.strataMaterial(105, 100)) == "limestone"); // above the pile
    CHECK(name(geology.strataMaterial(95, 100)) == "limestone");
    CHECK(name(geology.strataMaterial(85, 100)) == "shale");
    CHECK(name(geology.strataMaterial(60, 100)) == "granite");
    // Full lookup (no folds, faults or intrusions here): uplift moves the strata.
    CHECK(name(geology.material(0, 0, 85, 10, 100, 0)) == "shale"); // uplifted 10 m: rock from 75 m
    CHECK(name(geology.material(0, 0, 95, 10, 100, 0)) == "shale");
}

void testGeologyDeformation() {
    using J = nlohmann::json;
    const auto params = wg::WorldConfig::fromJson(
                            {{"seed", 1},
                             {"tectonics", {{"phase_count", 0}}},
                             {"geology",
                              {{"strata", J::array({{{"material", "limestone"}, {"thickness_m", 50}},
                                                    {{"material", "shale"}, {"thickness_m", 50}}})},
                               {"basement_material", "gneiss"},
                               {"strata_top_m", 100},
                               {"folding", {{"enabled", true}, {"axis_angle_deg", 0}, {"wavelength_m", 400},
                                            {"amplitude_m", 50}, {"asymmetry", 0}, {"plunge_deg", 0}}},
                               {"faults", J::array({{{"x", 0}, {"y", 0}, {"angle_deg", 0}, {"length_m", 1000},
                                                     {"dip_deg", 60}, {"offset_m", 30}}})},
                               {"fault_tip_taper", 0.2},
                               {"intrusions", J::array({{{"material", "granite"}, {"x", 2000}, {"y", 0},
                                                         {"radius_m", 100}, {"top_m", 50}, {"dome_k", 0.5}}})}}}})
                            .island();
    const wg::Geology geology(params);
    const auto name = [&](int id) { return geology.materialNames()[id]; };

    // Fold: amplitude * sin(2 pi v / wavelength), v across the axis (here y).
    CHECK(std::abs(geology.foldOffset(0, 100) - 50.0) < 1e-9);
    CHECK(std::abs(geology.foldOffset(123, 0)) < 1e-9);

    // Fault along x: the left side (+y) is raised by 30 m; the plane dips
    // towards -y, so at depth the raised block extends under the -y side.
    CHECK(geology.faultOffset(0, 10, 0) == 30.0);
    CHECK(geology.faultOffset(0, -10, 0) == 0.0);
    CHECK(geology.faultOffset(0, -10, -100) == 30.0); // 100 m deep, the plane is 58 m towards -y
    CHECK(geology.faultOffset(600, 10, 0) == 0.0);     // beyond its length
    CHECK(geology.faultOffset(480, 10, 0) > 0.0 && geology.faultOffset(480, 10, 0) < 30.0); // tapered tip

    // Same altitude on both sides of the fault: the raised side shows deeper
    // strata (fold offset passed as 0 to isolate the fault).
    CHECK(name(geology.material(0, -10, 40, 0, 100, 0)) == "shale");     // depth 60
    CHECK(name(geology.material(0, 10, 40, 0, 100, 0)) == "shale");      // depth 90
    CHECK(name(geology.material(0, 10, 15, 0, 100, 0)) == "gneiss");     // depth 115
    CHECK(name(geology.material(0, -10, 15, 0, 100, 0)) == "shale");     // depth 85

    // Intrusion: a dome at x = 2000 whose top (50 m) drops by 0.5 * radius at its edge.
    CHECK(name(geology.material(2000, 0, 40, 0, 100, 0)) == "granite");
    CHECK(name(geology.material(2090, 0, 20, 0, 100, 0)) != "granite"); // top 9.5 m at d = 90
    CHECK(name(geology.material(2090, 0, 5, 0, 100, 0)) == "granite");
}

void testDerivedFaultsFollowTheirPhase() {
    using J = nlohmann::json;
    const auto params = wg::WorldConfig::fromJson(
                            {{"seed", 1},
                             {"geology", {{"fault_count", 0}}},
                             {"tectonics", {{"phases", J::array({{{"type", "block_uplift"}, {"center", {0, 0}},
                                                                   {"angle_deg", 0}, {"length_m", 600}, {"width_m", 300},
                                                                   {"rate_m_per_year", 1e-4}, {"start_years", 0},
                                                                   {"end_years", 100000}}})}}}})
                            .island();
    wg::Geology geology(params);
    CHECK(geology.faultOffset(0, 0, 0) == 0.0); // the phase has not acted yet
    geology.setPhaseProgress({0.5});
    CHECK(std::abs(geology.faultOffset(0, 0, 0) - 5.0) < 1e-9); // half of 1e-4 * 1e5
    geology.setPhaseProgress({1.0});
    CHECK(std::abs(geology.faultOffset(0, 0, 0) - 10.0) < 1e-9);
    CHECK(geology.faultOffset(0, 400, 0) == 0.0); // outside the block
    CHECK(geology.faultOffset(0, 160, 0) == 0.0); // just outside at sea level...
    const double dip = params.tectonics.phases[0].faultDipDeg * std::numbers::pi / 180.0;
    CHECK(geology.faultOffset(0, 160, -20.0 * std::tan(dip)) == 10.0); // ...inside 20 m deeper: the walls dip outwards

    // Horst / graben: bands across the axis, raised and lowered by U.
    const auto bands = wg::WorldConfig::fromJson(
                           {{"seed", 1},
                            {"geology", {{"fault_count", 0}}},
                            {"tectonics", {{"phases", J::array({{{"type", "horst_graben"}, {"center", {0, 0}},
                                                                  {"angle_deg", 0}, {"length_m", 2000}, {"width_m", 200},
                                                                  {"rate_m_per_year", 1e-4}, {"start_years", 0},
                                                                  {"end_years", 100000}}})}}}})
                           .island();
    wg::Geology horsts(bands);
    horsts.setPhaseProgress({1.0});
    CHECK(horsts.faultOffset(0, 100, 0) == 10.0);  // band 0: horst
    CHECK(horsts.faultOffset(0, 300, 0) == -10.0); // band 1: graben
    CHECK(horsts.faultOffset(0, -100, 0) == -10.0);
}

void testVoxelVolume() {
    wg::VoxelVolume a(20, 20, -8, 30);
    a.fillBox(0, -8, 0, 20, 8, 20, wg::Block::Stone); // fully covers the first section layer
    CHECK(a.isSectionUniform(0) && a.sectionUniformBlock(0) == wg::Block::Stone);
    CHECK(a.get(19, 7, 19) == wg::Block::Stone && a.get(19, 8, 19) == wg::Block::Air);
    CHECK(a.topNonAir(3, 3) == 7);

    const wg::VoxelVolume b = a; // copy-on-write
    a.set(1, 20, 1, wg::Block::Grass);
    CHECK(a.get(1, 20, 1) == wg::Block::Grass && b.get(1, 20, 1) == wg::Block::Air);
    CHECK(a.get(99, 0, 0) == wg::Block::Air);

    a.set(1, 20, 1, wg::Block::Air);
    a.compact();
    CHECK(a.isSectionUniform(1));
}

void testWorldFileRoundTrip() {
    const auto config = makeConfig(nlohmann::json::array({{{"type", "parameters"}}}), 37, 21);
    wg::WorldState world = makePipeline(config).run(config).back().state;
    world.createLayer("height", 3.5f).at(36, 20) = 12.0f;
    world.createLayer("moisture", 0.25f);
    auto& voxels = world.createVoxels();
    voxels.fillBox(0, -64, 0, 37, 40, 21, wg::Block::Stone);
    CHECK(world.paramsHandle() == config.params);
    voxels.set(4, 100, 2, wg::Block::Snow);

    const auto path = std::filesystem::temp_directory_path() / "wg_roundtrip.wgw";
    wg::writeWorld(world, path);
    const auto loaded = wg::readWorld(path);
    std::filesystem::remove(path);

    CHECK(loaded.info().width == 37 && loaded.info().length == 21 && loaded.info().seed == 1);
    CHECK(loaded.hasParams() && loaded.params().toJson() == world.params().toJson());
    CHECK(loaded.layerNames() == world.layerNames());
    CHECK(loaded.layer("height").at(36, 20) == 12.0f && loaded.layer("moisture").at(0, 0) == 0.25f);
    CHECK(loaded.voxels().get(4, 100, 2) == wg::Block::Snow);
    CHECK(loaded.voxels().topNonAir(5, 5) == 39);
}

void testDeterminism() {
    CHECK(wg::deriveSeed(1, "flat") == wg::deriveSeed(1, "flat"));
    CHECK(wg::deriveSeed(1, "flat") != wg::deriveSeed(2, "flat"));
    CHECK(wg::deriveSeed(1, "flat") != wg::deriveSeed(1, "erosion"));
    // Pinned value: changing it silently changes every generated world.
    wg::Rng rng(0);
    CHECK(rng.nextU64() == 0xE220A8397B1DCDAFull);
}

} // namespace

int main() {
    testConfig();
    testParamsDeterminism();
    testParamsOverrideDoesNotShiftOthers();
    testParamsDependencies();
    testParamsLists();
    testParamsValidation();
    testParamsRoundTrip();
    testIslandParamsFile();
    testPipelineConfig();
    testSnapshots();
    testMissingLayerFails();
    testLiveCallbacks();
    testIslandInit();
    testIslandFitsInMap();
    testShapeByOrigin();
    testSharedAxis();
    testAtoll();
    testTopographyByOrigin();
    testUpliftPatterns();
    testTectonicsByOrigin();
    testGeology();
    testGeologyDeformation();
    testDerivedFaultsFollowTheirPhase();
    testVoxelVolume();
    testWorldFileRoundTrip();
    testDeterminism();
    if (failures == 0)
        std::puts("all tests passed");
    return failures == 0 ? 0 : 1;
}
