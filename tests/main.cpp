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
#include "wg/sim/Climate.hpp"
#include "wg/sim/Coastal.hpp"
#include "wg/sim/Fluvial.hpp"
#include "wg/sim/Glacier.hpp"
#include "wg/sim/Hillslope.hpp"
#include "wg/sim/Marine.hpp"
#include "wg/sim/Routing.hpp"
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

// Digs a round pit into "h" (and keeps the island flags), for the lake test.
class DigStage final : public wg::Stage {
public:
    explicit DigStage(wg::StageParams& params)
        : x_(params.get("x", 0.0)), y_(params.get("y", 0.0)), radius_(params.get("radius_m", 100.0)),
          depth_(params.get("depth_m", 30.0)) {}

    void run(wg::WorldState& world, const wg::StageContext&) override {
        const wg::IslandGrid grid(world.params().values);
        auto& h = world.editLayer("h");
        for (int j = 0; j < grid.ny; ++j)
            for (int i = 0; i < grid.nx; ++i) {
                const double d = std::hypot(grid.x(i) - x_, grid.y(j) - y_);
                if (d < radius_)
                    h.at(i, j) -= static_cast<float>(depth_ * (1.0 - d / radius_));
            }
    }

private:
    double x_, y_, radius_, depth_;
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
        r.add<DigStage>("dig");
        for (const std::string type : {"parameters", "island_init", "simulation"})
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

// Steps 1 and 2 only (the simulation has its own tests).
std::vector<wg::Snapshot> runIsland(nlohmann::json config) {
    if (!config.contains("pipeline"))
        config["pipeline"] = nlohmann::json::array({{{"type", "parameters"}}, {{"type", "island_init"}}});
    const auto loaded = wg::WorldConfig::fromJson(config);
    return wg::Pipeline::fromConfig(loaded).run(loaded);
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
        const double roughness = coral.params().values.initialTopography.roughnessM; // + the smooth floor's 0.25 m
        CHECK(landMax <= a.ringHeightM + a.ringNoiseM + roughness + 0.26);
        // (lagoon cells touching the ring are the shore: sea level + the 1 mm jitter)
        CHECK(lagoonMin >= -(a.lagoonDepthM + a.lagoonNoiseM) - 0.01 && lagoonMax < 1e-3f);

        // Barrier: low, below base + crest + noise.
        const auto barrier = islandOf(seed, "sedimentary");
        const auto& t = barrier.params().values.initialTopography;
        const auto [bMin, bMax] = wg::valueRange(barrier.layer(wg::layers::kHeight));
        CHECK(t.mode == "barrier" && bMax > 0.0f);
        CHECK(bMax <= t.baseElevationM + t.barrier.crestHeightM + t.noise.amplitudeM + t.roughnessM + 0.26);
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
    phases[1]["negative_factor"] = 1.0; // grabens as deep as the horsts are high (default 0.3, see the tilt)
    // A big round flat island so that the mask is 1 around the center.
    const auto state = runIsland({{"seed", 2},
                                  {"world", {{"width", 1024}, {"length", 1024}}},
                                  {"simulation", {{"coarse_resolution", 256}}},
                                  {"origin", {{"type", "volcanic"}}},
                                  {"shape", {{"lobe_count", 1}, {"lobe_spread", 0.0}, {"radius_ratio", 0.8}}},
                                  // exact shapes: no warp, no modulation
                                  {"tectonics", {{"phases", phases}, {"uplift_warp_amplitude", 0}, {"uplift_modulation", 0}}}})[1]
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
    CHECK(at(2, 0, 50) > 0.45f && at(2, 0, 50) < 0.55f);                           // tilt
    CHECK(at(2, 0, -200) < -0.25f && at(2, 0, -200) > -0.35f);                     // its lowered side, x 0.3
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

// A drawn phase that sinks the island is dropped, and the resolved config
// still reproduces the parameters exactly.
void testSinkingPhaseDropped() {
    const nlohmann::json config = {{"seed", 114}, {"origin", {{"type", "continental"}}}};
    const auto original = wg::resolveIslandParams(114, config);
    CHECK(original.notes.size() == 1);
    CHECK(original.values.tectonics.phases.size() ==
          static_cast<std::size_t>(original.toJson()["tectonics"]["phase_count"].get<int>()));
    const auto replayed = wg::resolveIslandParams(114, nlohmann::json::parse(original.toJson().dump()));
    CHECK(replayed.notes.empty()); // nothing left to drop
    // (same values; the key order may differ: it follows the resolution order)
    CHECK(nlohmann::json(replayed.toJson()) == nlohmann::json(original.toJson()));
    // Forced types are never dropped.
    nlohmann::json forced = config;
    forced["tectonics"]["phases"] = nlohmann::json::array();
    for (const auto& phase : original.toJson()["tectonics"]["phases"])
        forced["tectonics"]["phases"].push_back({{"type", phase["type"]}});
    forced["tectonics"]["phases"].push_back({{"type", "subsidence"}, {"rate_m_per_year", 5e-3}});
    CHECK(wg::resolveIslandParams(114, forced).values.tectonics.phases.size() == forced["tectonics"]["phases"].size());
}

// Durations and uplift (step 3, part 1 fixes): uplift and erosion of the same order.
void testDurationsAndUplift() {
    int ongoing = 0;
    const int seeds = 200;
    for (int seed = 1; seed <= seeds; ++seed) {
        const auto p = wg::WorldConfig::fromJson({{"seed", seed}, {"origin", {{"type", "continental"}}}}).island();
        const double duration = p.simulation.totalDurationYears;
        CHECK(duration >= 2e5 && duration <= 2e6);
        const auto& t = p.tectonics;
        const double expected = t.targetReliefM * std::max(p.erosion.fluvial.kBase *
                                                               std::pow(p.climate.basePrecipitationMPerYear, p.erosion.fluvial.m) / 10.0,
                                                           1.0 / duration);
        CHECK(std::abs(t.maxUpliftRateMPerYear - std::clamp(expected, 1e-5, 1e-2)) < 1e-9 * expected);
        if (!t.ongoingUplift)
            continue;
        ++ongoing;
        int last = -1;
        for (std::size_t k = 0; k < t.phases.size(); ++k)
            if (t.phases[k].type == "collision" || t.phases[k].type == "dome" || t.phases[k].type == "block_uplift")
                last = static_cast<int>(k);
        if (last >= 0)
            CHECK(t.phases[last].endYears == duration && t.phases[last].startYears <= 0.2 * duration);
    }
    CHECK(ongoing > 0.65 * seeds && ongoing < 0.85 * seeds); // ~75 %
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
    // A bell, not a cylinder: beyond its radius it continues, wider with depth.
    CHECK(name(geology.material(2150, 0, -50, 0, 100, 0)) != "granite"); // roof at -62.5 m at d = 150
    CHECK(name(geology.material(2150, 0, -100, 0, 100, 0)) == "granite");
}

void testDerivedFaultsFollowTheirPhase() {
    using J = nlohmann::json;
    const auto params = wg::WorldConfig::fromJson(
                            {{"seed", 1},
                             {"geology", {{"fault_count", 0}}},
                             {"tectonics", {{"uplift_warp_amplitude", 0}, {"uplift_modulation", 0},
                                            {"phases", J::array({{{"type", "block_uplift"}, {"center", {0, 0}},
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
    // The wall is in the middle of the soft edge, at least 15 m inside the outline (150 m).
    CHECK(geology.faultOffset(0, 140, 0) == 0.0); // just outside at sea level...
    const double dip = params.tectonics.phases[0].faultDipDeg * std::numbers::pi / 180.0;
    CHECK(geology.faultOffset(0, 140, -40.0 * std::tan(dip)) == 10.0); // ...inside 40 m deeper: the walls dip outwards

    // Horst / graben: bands across the axis, raised by U, lowered by 0.3 U.
    const auto bands = wg::WorldConfig::fromJson(
                           {{"seed", 1},
                            {"geology", {{"fault_count", 0}}},
                            {"tectonics", {{"uplift_warp_amplitude", 0}, {"uplift_modulation", 0}, {"phases", J::array({{{"type", "horst_graben"}, {"center", {0, 0}},
                                                                  {"angle_deg", 0}, {"length_m", 2000}, {"width_m", 200},
                                                                  {"rate_m_per_year", 1e-4}, {"start_years", 0},
                                                                  {"end_years", 100000}}})}}}})
                           .island();
    wg::Geology horsts(bands);
    horsts.setPhaseProgress({1.0});
    CHECK(horsts.faultOffset(0, 100, 0) == 10.0);  // band 0: horst
    CHECK(std::abs(horsts.faultOffset(0, 300, 0) + 3.0) < 1e-9); // band 1: graben
    CHECK(std::abs(horsts.faultOffset(0, -100, 0) + 3.0) < 1e-9);
}

// --- Simulation (step 3) ---------------------------------------------------------------

// A round flat island lifted by one constant dome, eroded by rivers.
nlohmann::json simulationConfig(double kBase, double dt = 1e4, double duration = 2e6) {
    using J = nlohmann::json;
    return {{"seed", 5},
            {"world", {{"width", 2048}, {"length", 2048}}},
            {"origin", {{"type", "continental"}}},
            {"shape", {{"lobe_count", 1}, {"lobe_spread", 0.0}, {"radius_ratio", 0.6}, {"elongation", 1.0}}},
            {"initial_topography", {{"mode", "flat"}, {"base_elevation_m", 10}}},
            {"tectonics", {{"phases", J::array({{{"type", "dome"}, {"center", {0, 0}}, {"width_m", 400},
                                                  {"rate_m_per_year", 5e-4}, {"start_years", -1e7},
                                                  {"end_years", 1e8}}})}}},
            {"climate", {{"base_precipitation_m_per_year", 1.0}}},
            // rivers only: no climate (uniform rain), no wave erosion
            {"erosion", {{"fluvial", {{"k_base", kBase}, {"m", 0.5}}}, {"coastal", {{"wave_erosion_coeff", 0.0}}}}},
            {"simulation",
             {{"coarse_resolution", 128}, {"total_duration_years", duration}, {"dt_years", dt},
              {"log_interval_steps", 10}, {"processes", {{"climate", false}}}}},
            {"pipeline", J::array({{{"type", "parameters"}}, {{"type", "island_init"}}, {{"type", "simulation"}}})}};
}

struct SimulationRun {
    wg::WorldState state;
    std::vector<double> meanRates; // mean |dh/dt| of each log line
};

SimulationRun simulate(const nlohmann::json& config, const wg::StageRegistry& registry = testRegistry()) {
    const auto loaded = wg::WorldConfig::fromJson(config);
    SimulationRun run;
    wg::PipelineCallbacks callbacks;
    callbacks.onLog = [&](std::size_t, const std::string& line) {
        const auto at = line.find("mean |dh/dt| ");
        if (at != std::string::npos)
            run.meanRates.push_back(std::stod(line.substr(at + 13)));
    };
    run.state = wg::Pipeline::fromConfig(loaded, registry).run(loaded, callbacks).back().state;
    return run;
}

void testSimulationSteadyState() {
    const auto run = simulate(simulationConfig(2e-5));
    const auto& h = run.state.layer(wg::layers::kHeight);
    const auto& q = run.state.layer(wg::layers::kDischarge);
    CHECK(std::all_of(h.data().begin(), h.data().end(), [](float v) { return std::isfinite(v); }));
    // Relief built by the dome, then held by erosion: |dh/dt| decays towards 0.
    CHECK(run.meanRates.size() >= 10);
    CHECK(run.meanRates.back() < 0.2 * run.meanRates.front());
    // A river network: discharge concentrates far beyond a single cell's.
    const double cell = 1.0 * 16.0 * 16.0;
    CHECK(wg::valueRange(q).max > 200.0 * cell);
    CHECK(run.state.scalar("time_years") == 2e6 && run.state.scalar("steps") == 200);

    // Calibration: twice the erodibility, lower equilibrium relief.
    const auto softer = simulate(simulationConfig(4e-5));
    CHECK(wg::valueRange(softer.state.layer(wg::layers::kHeight)).max < 0.8f * wg::valueRange(h).max);

    // Deterministic.
    const auto again = simulate(simulationConfig(2e-5));
    const auto a = h.data(), b = again.state.layer(wg::layers::kHeight).data();
    CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));

    // FIXED cells untouched.
    const auto& flags = run.state.byteLayer(wg::layers::kFlags);
    const float boundary = static_cast<float>(run.state.params().values.simulation.boundaryDepthM);
    for (std::size_t k = 0; k < h.size(); ++k)
        if (flags.data()[k] & wg::layers::kFixed)
            CHECK(h.data()[k] == boundary);
}

void testSimulationLake() {
    // A pit dug in the island becomes a lake that the drainage crosses.
    auto config = simulationConfig(2e-5, 1e4, 1e4);
    config["pipeline"] = nlohmann::json::array({{{"type", "parameters"}}, {{"type", "island_init"}},
                                                {{"type", "dig"}, {"x", 100}, {"y", 0}, {"radius_m", 200}},
                                                {{"type", "simulation"}}});
    const auto run = simulate(config);
    const wg::IslandGrid grid(run.state.params().values);
    const int i = static_cast<int>(100.0 / grid.cellM + grid.nx / 2), j = grid.ny / 2;
    const auto& lake = run.state.layer(wg::layers::kLakeDepth);
    const auto& q = run.state.layer(wg::layers::kDischarge);
    CHECK(lake.at(i, j) > 10.0f);
    // The water crosses the lake: all its cells drain to the spill point, so the
    // lake cell next to it carries at least the rain fallen on the whole lake.
    int lakeCells = 0;
    float maxQ = 0.0f;
    for (std::size_t k = 0; k < lake.size(); ++k) {
        if (lake.data()[k] > 0.01f) {
            ++lakeCells;
            maxQ = std::max(maxQ, q.data()[k]);
        }
    }
    const double cellRain = 1.0 * grid.cellM * grid.cellM;
    CHECK(lakeCells > 100 && maxQ >= 0.999 * lakeCells * cellRain);
    // Only the pit is a lake: the epsilon slope across the flat plateau around
    // it (centimetres over hundreds of cells) is not water.
    const double pitCells = 3.14159 * 200.0 * 200.0 / (grid.cellM * grid.cellM);
    CHECK(lakeCells < 1.5 * pitCells);
}

// --- Hillslopes (step 3, part 2) ------------------------------------------------------

void testRegolith() {
    std::vector<double> b{100.0}, sed{0.0};
    std::vector<wg::Composition> frac(1);
    const std::vector<int> kind{wg::kClay};
    const wg::Regolith regolith{b, sed, frac, kind};
    regolith.deposit(0, {2.0, 0.0, 0.0}); // 2 m of gravel
    regolith.deposit(0, {0.0, 2.0, 0.0}); // + 2 m of sand: half and half
    CHECK(sed[0] == 4.0 && frac[0][wg::kGravel] == 0.5 && frac[0][wg::kSand] == 0.5);
    // Removal takes the sediment first (with its composition), then the rock.
    const auto removed = regolith.remove(0, 5.0);
    CHECK(sed[0] == 0.0 && b[0] == 99.0);
    CHECK(removed[wg::kGravel] == 2.0 && removed[wg::kSand] == 2.0 && removed[wg::kClay] == 1.0);
}

void testHillslopeDiffusion() {
    const int nx = 64, ny = 48, n = nx * ny;
    std::vector<double> h(n);
    std::vector<std::uint8_t> active(n);
    for (int c = 0; c < n; ++c) {
        const int i = c % nx, j = c / nx;
        active[c] = std::hypot(i - 30, j - 24) < 20; // an island; the rest keeps its height
        h[c] = active[c] ? 10.0 + 5.0 * ((i * 7 + j * 13) % 5) : -50.0;
    }
    const auto sum = [&](const std::vector<double>& v) {
        double total = 0.0;
        for (int c = 0; c < n; ++c)
            total += active[c] ? v[c] : 0.0;
        return total;
    };
    const std::vector<double> before = h;
    const int substeps = wg::diffuseADI(h, active, nx, ny, 7.5);
    CHECK(substeps == 8); // r = 7.5: sub-steps of r <= 1
    CHECK(std::abs(sum(h) - sum(before)) < 1e-9 * sum(before)); // no flux out of the island
    double lo = 1e9, hi = -1e9, variance = 0.0, varianceBefore = 0.0;
    const double mean = sum(h) / std::count(active.begin(), active.end(), 1);
    for (int c = 0; c < n; ++c) {
        if (!active[c]) {
            CHECK(h[c] == -50.0);
            continue;
        }
        lo = std::min(lo, h[c]);
        hi = std::max(hi, h[c]);
        variance += (h[c] - mean) * (h[c] - mean);
        varianceBefore += (before[c] - mean) * (before[c] - mean);
    }
    CHECK(lo >= 10.0 - 1e-9 && hi <= 30.0 + 1e-9); // no new extrema
    CHECK(variance < 0.01 * varianceBefore);      // the grid-scale noise is gone
}

void testThermalCollapse() {
    // A 100 m rock cliff (45 degrees repose) collapses into scree at its foot.
    const int nx = 64, ny = 8, n = nx * ny;
    const double dx = 4.0;
    std::vector<double> b(n), sed(n, 0.0), h(n), rockTan(n, 1.0);
    std::vector<wg::Composition> frac(n);
    const std::vector<int> kind(n, wg::kSand);
    const std::vector<std::uint8_t> source(n, 1), fixed(n, 0);
    for (int c = 0; c < n; ++c)
        h[c] = b[c] = (c % nx) < 32 ? 150.0 : 50.0;
    const wg::Regolith regolith{b, sed, frac, kind};
    const double tan33 = std::tan(33.0 * std::numbers::pi / 180.0);
    double volume = 0.0;
    for (double v : h)
        volume += v;
    for (int pass = 0; pass < 400; ++pass)
        wg::thermalCollapse(regolith, h, source, fixed, rockTan, tan33, 0.5, nx, ny, dx);
    double after = 0.0, worstRock = 0.0, worstScree = 0.0;
    for (double v : h)
        after += v;
    CHECK(std::abs(after - volume) < 1e-9 * volume);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            const int c = j * nx + i;
            const double slope = std::abs(h[c] - h[c + 1]) / dx;
            if (sed[c] < 1e-6 && sed[c + 1] < 1e-6)
                worstRock = std::max(worstRock, slope);
            else if (sed[c] > 1.0 && sed[c + 1] > 1.0)
                worstScree = std::max(worstScree, slope / tan33);
        }
    }
    CHECK(worstRock < 1.1);   // rock: at most ~45 degrees
    CHECK(worstScree < 1.15); // scree: at most ~33 degrees
    CHECK(sed[4 * nx + 34] > 1.0 && sed[4 * nx + 10] == 0.0); // scree below the cliff, none on the plateau
    CHECK(frac[4 * nx + 34][wg::kSand] == 1.0);
}

void testSimulationSediment() {
    auto config = simulationConfig(2e-5, 1e4, 5e5);
    config["erosion"]["hillslope"] = {{"diffusivity_m2_per_year", 0.02}};
    config["erosion"]["thermal"] = {{"iterations_per_step", 2}};
    std::vector<double> volumeErrors, budgetDrifts;
    const auto loaded = wg::WorldConfig::fromJson(config);
    wg::PipelineCallbacks callbacks;
    callbacks.onLog = [&](std::size_t, const std::string& line) {
        const auto at = line.find("hillslope dV/V ");
        if (at != std::string::npos)
            volumeErrors.push_back(std::stod(line.substr(at + 15)));
        const auto drift = line.find("drift ");
        if (drift != std::string::npos)
            budgetDrifts.push_back(std::abs(std::stod(line.substr(drift + 6))));
    };
    const auto state = wg::Pipeline::fromConfig(loaded, testRegistry()).run(loaded, callbacks).back().state;
    CHECK(!volumeErrors.empty());
    for (double e : volumeErrors)
        CHECK(e < 1e-10);
    CHECK(!budgetDrifts.empty());
    for (double e : budgetDrifts)
        CHECK(e < 1e-9); // rock eroded = stored + exported
    const auto& sed = state.layer(wg::layers::kSediment);
    const auto& gravel = state.layer(wg::layers::kSedimentGravel);
    const auto& sand = state.layer(wg::layers::kSedimentSand);
    const auto& clay = state.layer(wg::layers::kSedimentClay);
    const auto& dominant = state.layer(wg::layers::kSedimentDominant);
    int covered = 0;
    for (std::size_t k = 0; k < sed.size(); ++k) {
        CHECK(sed.data()[k] >= 0.0f);
        if (sed.data()[k] > 0.01f) {
            ++covered;
            CHECK(std::abs(gravel.data()[k] + sand.data()[k] + clay.data()[k] - 1.0f) < 1e-4f);
            CHECK(dominant.data()[k] >= 1.0f);
        }
    }
    CHECK(covered > 10); // scree and colluvium somewhere
    const auto& material = state.layer(wg::layers::kSurfaceMaterial);
    const auto categories = state.layerMeta(wg::layers::kSurfaceMaterial).categories.size();
    for (float id : material.data())
        CHECK(id >= 0.0f && id < static_cast<float>(categories));
}

// --- Sediment transport (step 3, part 3) ---------------------------------------------

// A cone of rock in the sea, routed once.
struct ConeWorld {
    int nx = 48, ny = 40, n = 48 * 40;
    double dx = 10.0;
    std::vector<double> h, b, sed;
    std::vector<wg::Composition> frac;
    std::vector<int> kind;
    std::vector<std::uint8_t> fixed;
    ConeWorld() : h(n), b(n), sed(n, 0.0), frac(n), kind(n, wg::kSand), fixed(n, 0) {
        for (int c = 0; c < n; ++c) {
            const int i = c % nx, j = c / nx;
            const double r = std::hypot(i - 24.0, j - 20.0);
            b[c] = 60.0 - 4.0 * r + 0.3 * ((i * 7 + j * 13) % 5); // roughness for a network
            fixed[c] = i == 0 || j == 0 || i == nx - 1 || j == ny - 1;
            if (fixed[c])
                b[c] = -40.0;
            h[c] = b[c];
        }
    }
};

void testFluvialRegression() {
    // G = 0: exactly the erosion-only implicit scheme of part 1 (uniform rock).
    ConeWorld w;
    wg::FlowRouter router(w.nx, w.ny, w.dx);
    router.route(w.h, w.fixed, 0.0);
    std::vector<double> q, area;
    router.accumulate(std::vector<double>(w.n, 1.0 * w.dx * w.dx), q);
    router.accumulate(std::vector<double>(w.n, w.dx * w.dx), area);
    const double k = 2e-5, dt = 1e4;
    std::vector<double> expected = w.h;
    for (const int c : router.stack()) {
        const int r = router.receivers()[c];
        if (r == c || w.fixed[c] || router.ocean()[c])
            continue;
        const double base = router.ocean()[r] ? std::max(expected[r], 0.0) : expected[r];
        if (expected[c] <= base)
            continue;
        const double f = k * std::sqrt(q[c]) * dt / router.receiverDistance()[c];
        expected[c] = (expected[c] + f * base) / (1.0 + f);
    }
    const wg::Regolith regolith{w.b, w.sed, w.frac, w.kind};
    wg::FluvialSettings settings;
    settings.kSediment = 5 * k;
    settings.m = 0.5;
    settings.g = 0.0;
    settings.dt = dt;
    settings.cellM = w.dx;
    std::vector<double> qs(w.n), erosion(w.n), deposition(w.n);
    const auto result = wg::fluvialStep(router, settings, regolith, q, area, std::vector<double>(w.n, k), w.fixed, w.h,
                                        qs, erosion, deposition);
    CHECK(result.iterations == 1);
    double worst = 0.0, deposited = 0.0;
    for (int c = 0; c < w.n; ++c) {
        if (!router.ocean()[c] && !w.fixed[c]) {
            worst = std::max(worst, std::abs(w.h[c] - expected[c]));
        }
        deposited += w.sed[c];
    }
    CHECK(worst < 1e-9); // land: identical to part 1
    // Nothing is lost: what the rock lost is at sea or exported.
    double rockLost = 0.0;
    const ConeWorld initial;
    for (int c = 0; c < w.n; ++c)
        if (!w.fixed[c])
            rockLost += initial.b[c] - w.b[c];
    CHECK(std::abs(rockLost * w.dx * w.dx - deposited * w.dx * w.dx - result.exported) < 1e-6 * rockLost * w.dx * w.dx);
    CHECK(result.toSea > 0.0);
}

void testFluvialDeposition() {
    // G > 0: the flux settles on the way (G Qs / A), the higher G the more.
    double previous = -1.0;
    for (const double g : {0.0, 0.5, 2.0}) {
        ConeWorld w;
        wg::FlowRouter router(w.nx, w.ny, w.dx);
        router.route(w.h, w.fixed, 0.0);
        std::vector<double> q, area;
        router.accumulate(std::vector<double>(w.n, 1.0 * w.dx * w.dx), q);
        router.accumulate(std::vector<double>(w.n, w.dx * w.dx), area);
        const wg::Regolith regolith{w.b, w.sed, w.frac, w.kind};
        wg::FluvialSettings settings;
        settings.kSediment = 1e-4;
        settings.m = 0.5;
        settings.g = g;
        settings.dt = 1e4;
        settings.cellM = w.dx;
        std::vector<double> qs(w.n), erosion(w.n), deposition(w.n);
        const auto result = wg::fluvialStep(router, settings, regolith, q, area, std::vector<double>(w.n, 2e-5), w.fixed,
                                            w.h, qs, erosion, deposition);
        double onLand = 0.0;
        for (int c = 0; c < w.n; ++c) {
            CHECK(w.sed[c] >= 0.0 && std::abs(w.h[c] - w.b[c] - w.sed[c]) < 1e-9);
            if (!router.ocean()[c] && !w.fixed[c])
                onLand += w.sed[c];
        }
        CHECK(result.iterations <= settings.maxIterations);
        CHECK(onLand > previous || (g == 0.0 && onLand == 0.0));
        previous = onLand;
    }
}

void testMarineDiffusion() {
    // A load dropped at a river mouth spreads over the shelf; only sediment moves,
    // the clay farther than the sand.
    const int nx = 64, ny = 48, n = nx * ny;
    std::vector<double> b(n), sed(n, 0.0), h(n);
    std::vector<wg::Composition> frac(n, wg::Composition{0.0, 1.0, 0.0});
    const std::vector<int> kind(n, wg::kSand);
    std::vector<std::uint8_t> active(n, 0), fixed(n, 0);
    for (int c = 0; c < n; ++c) {
        const int i = c % nx, j = c / nx;
        b[c] = i < 48 ? -2.0 - 0.5 * (48 - i) : 5.0 + (i - 48); // land for i >= 48
        fixed[c] = i == 0 || j == 0 || i == nx - 1 || j == ny - 1;
        active[c] = !fixed[c] && i < 48;
    }
    const int mouth = 24 * nx + 47;
    sed[mouth] = 400.0;
    frac[mouth] = {0.0, 0.5, 0.5};
    for (int c = 0; c < n; ++c)
        h[c] = b[c] + sed[c];
    const std::vector<double> rock = b;
    const wg::Regolith regolith{b, sed, frac, kind};
    wg::MarineSettings settings;
    settings.sandD0 = 0.5;
    settings.finesD0 = 5.0;
    settings.depthScaleM = 20.0;
    settings.dt = 1e3;
    settings.cellM = 10.0;
    const double exported = wg::marineDiffusion(regolith, h, active, fixed, nx, ny, settings);
    double total = 0.0, sandDistance = 0.0, sand = 0.0, clayDistance = 0.0, clay = 0.0;
    for (int c = 0; c < n; ++c) {
        CHECK(b[c] == rock[c]);
        CHECK(sed[c] >= 0.0);
        if (!active[c] && c != mouth)
            CHECK(sed[c] == 0.0); // nothing climbs onto the land
        total += sed[c];
        const double d = std::hypot(c % nx - 47.0, c / nx - 24.0);
        sand += sed[c] * frac[c][wg::kSand];
        sandDistance += d * sed[c] * frac[c][wg::kSand];
        clay += sed[c] * frac[c][wg::kClay];
        clayDistance += d * sed[c] * frac[c][wg::kClay];
    }
    CHECK(std::abs(total + exported / 100.0 - 400.0) < 1e-9 * 400.0);
    CHECK(sed[mouth] < 50.0);                                  // spread
    CHECK(clayDistance / clay > 1.5 * sandDistance / sand);     // fines farther
}

void testLakeFilling() {
    // The pit of testSimulationLake fills with the sediment of the river.
    auto config = simulationConfig(2e-5, 1e4, 3e5);
    config["erosion"]["fluvial"]["deposition_g"] = 1.0;
    config["pipeline"] = nlohmann::json::array({{{"type", "parameters"}}, {{"type", "island_init"}},
                                                {{"type", "dig"}, {"x", 100}, {"y", 0}, {"radius_m", 200}},
                                                {{"type", "simulation"}}});
    const auto run = simulate(config);
    const wg::IslandGrid grid(run.state.params().values);
    const int i = static_cast<int>(100.0 / grid.cellM + grid.nx / 2), j = grid.ny / 2;
    CHECK(run.state.layer(wg::layers::kSediment).at(i, j) > 1.0f);
    CHECK(run.state.layer(wg::layers::kLakeDepth).at(i, j) < 18.0f); // dug 20 m deep
}

void testStrongDiffusionStaysBounded() {
    // Seed 3 at dx = 4 m: D * dt / dx2 ~ 26 hillslope sub-steps. With each sea
    // cell held at its own depth, coastal cells piped sediment from shallow to
    // deep sea cells every sub-step, and the island blew up to 1e11 m.
    const nlohmann::json config = {
        {"seed", 3},
        {"world", {{"width", 1024}, {"length", 1024}}},
        {"simulation", {{"coarse_resolution", 256}, {"total_duration_years", 1e5}, {"log_interval_steps", 5}}},
        {"pipeline", nlohmann::json::array({{{"type", "parameters"}}, {{"type", "island_init"}}, {{"type", "simulation"}}})}};
    const auto run = simulate(config);
    const float initialHi = wg::valueRange(run.state.layer(wg::layers::kHeightInitial)).max;
    const float initialLo = wg::valueRange(run.state.layer(wg::layers::kHeightInitial)).min;
    CHECK(wg::valueRange(run.state.layer(wg::layers::kHeight)).max < initialHi + 100.0f);
    CHECK(wg::valueRange(run.state.layer(wg::layers::kBedrock)).min >= initialLo - 1.0f);
}

// --- Climate and coasts (step 3, part 4) ---------------------------------------------

void testRainShadow() {
    // A north-south ridge in the middle of an island: the windward slope is wet,
    // the lee dry; turning the wind round swaps them. The land mean is the base rate.
    for (const double windFrom : {180.0, 0.0}) {
        auto params = wg::WorldConfig::fromJson(
                          {{"seed", 1},
                           {"climate", {{"base_temperature_c", 15}, {"humidity", 0.8}, {"base_precipitation_m_per_year", 1.0},
                                        {"wind", {{"direction_deg", windFrom}, {"variability_deg", 0}}},
                                        {"orographic", {{"condensation_rate", 1.0}, {"base_rate", 0.05}}}}}})
                          .island();
        const int nx = 120, ny = 60, n = nx * ny;
        const double dx = 20.0;
        std::vector<double> s(n);
        std::vector<std::uint8_t> sea(n);
        for (int c = 0; c < n; ++c) {
            const int i = c % nx;
            sea[c] = i < 20 || i >= 100;
            s[c] = sea[c] ? -20.0 : 800.0 * std::exp(-0.5 * std::pow((i - 60) / 10.0, 2));
        }
        const wg::ClimateModel climate(params, nx, ny, dx);
        std::vector<double> t, p;
        climate.temperature(s, 0.0, t);
        climate.precipitation(s, t, sea, params.climate.baseTemperatureC, p);
        double west = 0.0, east = 0.0, land = 0.0;
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int c = j * nx + i;
                if (i >= 40 && i < 55)
                    west += p[c];
                if (i > 65 && i <= 80)
                    east += p[c];
                if (!sea[c])
                    land += p[c];
            }
        CHECK(std::abs(land / (80 * ny) - 1.0) < 1e-6); // mean over the land = base rate
        if (windFrom == 180.0)
            CHECK(west > 3.0 * east); // westerly wind: wet west slope
        else
            CHECK(east > 3.0 * west);
        CHECK(t[60] < t[30]); // colder on the ridge (lapse rate)
    }
}

void testCoastalRetreat() {
    // A straight cliff facing the swell, hard rock in the north half, soft in the
    // south: the soft rock retreats faster, the cliff keeps its height, the
    // platform lies at platform_depth_m, and every cubic meter is accounted for.
    const int nx = 200, ny = 100, n = nx * ny;
    std::vector<double> b(n), sed(n, 0.0), h(n), hardness(n);
    std::vector<wg::Composition> frac(n);
    const std::vector<int> kind(n, wg::kSand);
    std::vector<std::uint8_t> sea(n), fixed(n, 0);
    for (int c = 0; c < n; ++c) {
        const int i = c % nx, j = c / nx;
        h[c] = b[c] = i < 50 ? -20.0 : 24.0;
        sea[c] = i < 50;
        hardness[c] = j < 50 ? 0.85 : 0.25;
    }
    double rock0 = 0.0;
    for (double v : b)
        rock0 += v;
    const wg::Regolith regolith{b, sed, frac, kind};
    wg::CoastalSettings s;
    s.waveErosionCoeff = 0.03;
    s.swellEnergy = 1.0;
    s.swellDirectionDeg = 180.0;
    s.fetchMaxM = 3000.0;
    s.platformDepthM = 6.0;
    s.exportThreshold = 0.4;
    s.dt = 2000.0;
    s.cellM = 8.0;
    wg::CoastalErosion coast(nx, ny);
    double exported = 0.0;
    for (int step = 0; step < 150; ++step) {
        if (step % 10 == 0)
            coast.updateExposure(h, sea, s);
        coast.step(regolith, h, sea, fixed, hardness, s);
        exported += coast.exported();
    }
    CHECK(coast.exposure().size() == static_cast<std::size_t>(n));
    const auto coastAt = [&](int j) {
        int i = 0;
        while (i < nx && h[j * nx + i] < 0.0)
            ++i;
        return i;
    };
    // Expected: 0.03 * 0.95 / hardness / 30 m per year over 300 ka: ~42 cells hard, ~140 soft.
    CHECK(coastAt(25) - 50 > 30 && coastAt(25) - 50 < 55);
    CHECK(coastAt(75) - 50 > 2 * (coastAt(25) - 50));
    CHECK(h[25 * nx + coastAt(25)] > 23.0);                    // full-height cliff
    CHECK(std::abs(b[25 * nx + coastAt(25) - 3] + 6.0) < 1e-9); // platform at -6 m
    double rock = 0.0, stored = 0.0;
    for (int c = 0; c < n; ++c) {
        rock += b[c];
        stored += sed[c];
    }
    CHECK(std::abs((rock0 - rock) * 64.0 - stored * 64.0 - exported) < 1e-6 * (rock0 - rock) * 64.0);
}

// --- Glaciers (step 3, part 5) ---------------------------------------------------------

void testGlacierMass() {
    // A cold cone: ice builds on it and flows down; every cubic meter is
    // accounted for (accumulated - ablated - calved = volume), the ice does not
    // cross a ridge it does not top, and it melts away when it gets warm.
    const auto params = wg::WorldConfig::fromJson({{"seed", 1}, {"erosion", {{"glacial", {{"sliding_coeff", 3e-9}}}}}}).island();
    const int nx = 160, ny = 120, n = nx * ny;
    const double dx = 10.0; // ice grid: 50 m cells, 32 x 24
    std::vector<double> h(n), rain(n, 1.0);
    for (int c = 0; c < n; ++c) {
        const int i = c % nx, j = c / nx;
        const double r = std::hypot(i - 50.0, j - 60.0);
        h[c] = std::max(-100.0, 600.0 - 12.0 * r); // a cone at x = 50
        if (i >= 100 && i < 104)
            h[c] = 2000.0; // a wall, east of the cone
        if (i >= 104)
            h[c] = 0.0;    // a low plain behind it
    }
    wg::GlacierModel glacier(params, nx, ny, dx);
    const wg::GlacierModel::Climate cold{-2.0, 0.0, 6.5, 0.0, 0.0};
    glacier.relax(h, rain, cold);
    CHECK(glacier.hasIce());
    const auto& b = glacier.budget();
    const double expected = b.accumulated - b.ablated - b.calved - b.melted + b.clamped;
    CHECK(std::abs(glacier.volume() - expected) < 1e-6 * b.accumulated);
    CHECK(b.clamped < 1e-3 * b.accumulated);
    std::vector<double> ice, sliding, melt, balance;
    glacier.fineFields(ice, sliding, melt, balance);
    double behindWall = 0.0;
    for (int j = 0; j < ny; ++j)
        for (int i = 110; i < nx; ++i)
            behindWall += ice[j * nx + i];
    // (the plain gets its own snow: compare with the ice on the cone side)
    double coneSide = 0.0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < 100; ++i)
            coneSide += ice[j * nx + i];
    CHECK(coneSide > 0.0);
    CHECK(*std::max_element(sliding.begin(), sliding.end()) < 3e-9 * 1.5e5 * 1.5e5 * 1.0001); // yield stress cap
    glacier.meltAll(1000.0);
    CHECK(!glacier.hasIce());
    CHECK(std::abs(glacier.volume()) < 1e-9 && glacier.budget().melted > 0.0);
    (void)behindWall;
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
    testSinkingPhaseDropped();
    testDurationsAndUplift();
    testGeology();
    testGeologyDeformation();
    testDerivedFaultsFollowTheirPhase();
    testSimulationSteadyState();
    testSimulationLake();
    testRegolith();
    testHillslopeDiffusion();
    testThermalCollapse();
    testSimulationSediment();
    testFluvialRegression();
    testFluvialDeposition();
    testMarineDiffusion();
    testLakeFilling();
    testStrongDiffusionStaysBounded();
    testRainShadow();
    testCoastalRetreat();
    testGlacierMass();
    testVoxelVolume();
    testWorldFileRoundTrip();
    testDeterminism();
    if (failures == 0)
        std::puts("all tests passed");
    return failures == 0 ? 0 : 1;
}
