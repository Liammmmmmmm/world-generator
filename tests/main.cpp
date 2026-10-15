// Minimal self-contained tests (no framework yet). Run: ctest --test-dir build

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <filesystem>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/erosion/Avalanche.hpp"
#include "wg/erosion/Drop.hpp"
#include "wg/erosion/Lakes.hpp"
#include "wg/erosion/Rock.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/gen/StageRegistry.hpp"
#include "wg/io/MinecraftExport.hpp"
#include "wg/io/Nbt.hpp"
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
    CHECK(overridden.values.climate.wind.directionDeg == base.values.climate.wind.directionDeg);
    CHECK(overridden.values.geology.folding.asymmetry == base.values.geology.folding.asymmetry);
    // Dependent parameters follow the override (sizes are relative to the island radius).
    CHECK(base.values.shape.radiusRatio != 0.5);
    CHECK(overridden.values.initialTopography.volcano.radiusM != base.values.initialTopography.volcano.radiusM);
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

    const auto counted = resolve({{"seed", 5}, {"geology", {{"strata_count", 3}}}});
    CHECK(entry(counted, "geology.strata_count")->value == 3 && counted.values.geology.strata.size() == 3);

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
    for (const auto& e : replayed.entries)
        CHECK(e.source == wg::ParamSource::Config);
}

void testIslandParamsFile() {
    const auto config = wg::WorldConfig::load(WG_SOURCE_DIR "/island_params.jsonc");
    CHECK(config.seed == config.raw["seed"].get<std::uint64_t>() && config.info().width == 4096);
    CHECK(config.island().simulation.resolution == 1024);
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

// Parameters and island initialization only (unless the config has a pipeline).
std::vector<wg::Snapshot> runIsland(nlohmann::json config) {
    if (!config.contains("pipeline"))
        config["pipeline"] = {{{"type", "parameters"}}, {{"type", "island_init"}}};
    const auto loaded = wg::WorldConfig::fromJson(config);
    return wg::Pipeline::fromConfig(loaded).run(loaded);
}

void testIslandInit() {
    const nlohmann::json config = {{"seed", 9},
                                   {"world", {{"width", 512}, {"length", 512}}},
                                   {"simulation", {{"resolution", 128}}}};
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
                                          {"simulation", {{"resolution", 128}}}});
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
                          {"simulation", {{"resolution", 128}}},
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
                          {"simulation", {{"resolution", 256}}},
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

// Geology without folds: only the strata.
const nlohmann::json kPlainGeology = {{"folding", {{"enabled", false}}}};

wg::WorldState islandOf(int seed, const char* origin, nlohmann::json topography = nlohmann::json::object(),
                        nlohmann::json geology = nlohmann::json::object()) {
    return runIsland({{"seed", seed},
                      {"world", {{"width", 2048}, {"length", 2048}}},
                      {"simulation", {{"resolution", 256}}},
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
        // Strata parallel to the surface: the top layer everywhere.
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

void testReliefByOrigin() {
    int withRange = 0;
    for (int seed = 1; seed <= 20; ++seed) {
        for (const char* origin : {"sedimentary", "coral"})
            CHECK(wg::WorldConfig::fromJson({{"seed", seed}, {"origin", {{"type", origin}}}}).island().relief.zones.empty());
        const auto volcanic = wg::WorldConfig::fromJson({{"seed", seed}, {"origin", {{"type", "volcanic"}}}}).island();
        CHECK(volcanic.relief.zones.size() <= 1);
        for (const auto& z : volcanic.relief.zones)
            CHECK(z.type == "hills");
        const auto p = wg::WorldConfig::fromJson({{"seed", seed}, {"origin", {{"type", "continental"}}}}).island();
        CHECK(!p.relief.zones.empty() && p.relief.zones.size() <= 3);
        const auto range = std::find_if(p.relief.zones.begin(), p.relief.zones.end(),
                                        [](const auto& z) { return z.type == "range"; });
        if (range != p.relief.zones.end()) {
            ++withRange;
            CHECK(range->angleDeg == p.shape.mainAxisDeg); // the first range follows the main axis
        }
    }
    CHECK(withRange >= 8 && withRange <= 18); // about 60 %
}

void testReliefZones() {
    using J = nlohmann::json;
    // One range along x on a big round island: the relief is longer than wide,
    // and absent at sea.
    const auto state = runIsland({{"seed", 3},
                                  {"world", {{"width", 2048}, {"length", 2048}}},
                                  {"simulation", {{"resolution", 256}}},
                                  {"origin", {{"type", "continental"}}},
                                  {"shape", {{"lobe_count", 1}, {"lobe_spread", 0.0}, {"radius_ratio", 0.8},
                                             {"elongation", 1.0}, {"bay_count", 0}}},
                                  {"relief", {{"modulation", 0.0},
                                              {"zones", J::array({{{"type", "range"}, {"center", {0, 0}},
                                                                   {"angle_deg", 0}, {"length_m", 1200},
                                                                   {"width_m", 300}, {"height_m", 300}}})}}}})[1]
                           .state;
    const auto& relief = state.layer(wg::layers::kRelief);
    const auto& mask = state.layer(wg::layers::kMask);
    const wg::IslandGrid grid(state.params().values);
    double along = 0.0, across = 0.0;
    bool dryAtSea = true;
    for (int j = 0; j < grid.ny; ++j) {
        for (int i = 0; i < grid.nx; ++i) {
            const double x = grid.x(i), y = grid.y(j);
            if (std::abs(y) < 50 && std::abs(x) > 300 && std::abs(x) < 400)
                along += relief.at(i, j);
            if (std::abs(x) < 50 && std::abs(y) > 300 && std::abs(y) < 400)
                across += relief.at(i, j);
            dryAtSea = dryAtSea && (mask.at(i, j) > 0.0f || relief.at(i, j) == 0.0f);
        }
    }
    CHECK(along > 3.0 * across && dryAtSea);
    const auto [min, max] = wg::valueRange(relief);
    CHECK(min >= 0.0f && max > 50.0f && max <= 300.0f);
}

void testErosionPresets() {
    const auto erosion = [](const char* origin, nlohmann::json particles = nlohmann::json::object()) {
        return wg::WorldConfig::fromJson(
                   {{"seed", 1}, {"origin", {{"type", origin}}}, {"erosion", {{"particles", particles}}}})
            .island()
            .erosion.particles;
    };
    const auto continental = erosion("continental"), volcanic = erosion("volcanic");
    CHECK(continental.steps >= 4000 && continental.steps <= 5000 && continental.entrainment == 10.0);
    CHECK(volcanic.steps >= 2000 && volcanic.steps <= 5000 && volcanic.entrainment > 10.0);
    CHECK(erosion("sedimentary").steps < 200 && erosion("coral").steps < 200);
    // Presets are defaults: the config wins.
    CHECK(erosion("coral", {{"steps", 3000}, {"entrainment", 7.5}}).steps == 3000);
    CHECK(erosion("volcanic", {{"entrainment", 7.5}}).entrainment == 7.5);
}

void testSharedAxis() {
    const auto p = wg::WorldConfig::fromJson({{"seed", 4}, {"origin", {{"type", "continental"}}}}).island();
    CHECK(p.geology.folding.axisAngleDeg == p.shape.mainAxisDeg);

    // Forcing one of them leaves the others alone.
    const auto forced = wg::WorldConfig::fromJson({{"seed", 4},
                                                   {"origin", {{"type", "continental"}}},
                                                   {"geology", {{"folding", {{"axis_angle_deg", 12.0}}}}}})
                            .island();
    CHECK(forced.geology.folding.axisAngleDeg == 12.0 && forced.shape.mainAxisDeg == p.shape.mainAxisDeg);
}

void testGeology() {
    auto params = wg::WorldConfig::fromJson(
                      {{"seed", 1},
                       {"geology", {{"strata", {{{"material", "limestone"}, {"thickness_m", 10}},
                                                {{"material", "shale"}, {"thickness_m", 20}}}},
                                    {"basement_material", "granite"},
                                    {"strata_top_m", 100},
                                    {"folding", {{"enabled", false}}}}},
                       {"origin", {{"type", "continental"}}}})
                      .island();
    const wg::Geology geology(params);
    const auto name = [&](int id) { return geology.materialNames()[id]; };
    CHECK(name(geology.strataMaterial(105, 100)) == "limestone"); // above the pile
    CHECK(name(geology.strataMaterial(95, 100)) == "limestone");
    CHECK(name(geology.strataMaterial(85, 100)) == "shale");
    CHECK(name(geology.strataMaterial(60, 100)) == "granite");
    // Full lookup (no folds here): horizontal strata from strata_top_m.
    CHECK(name(geology.material(0, 0, 95)) == "limestone");
    CHECK(name(geology.material(1000, -700, 85)) == "shale");
    CHECK(name(geology.material(0, 0, 60)) == "granite");

    // Volcanic island: strata parallel to the initial surface.
    params.origin.type = "volcanic";
    const wg::Grid2D<float> surface(4, 4, 200.0f);
    const wg::Geology volcanic(params, &surface);
    CHECK(name(volcanic.material(0, 0, 195)) == "limestone");
    CHECK(name(volcanic.material(500, 300, 185)) == "shale");
    CHECK(name(volcanic.material(0, 0, 95)) == "granite");
    CHECK(name(wg::Geology(params).material(0, 0, 95)) == "limestone"); // no surface: horizontal
}

void testGeologyFolds() {
    using J = nlohmann::json;
    const auto params = wg::WorldConfig::fromJson(
                            {{"seed", 1},
                             {"origin", {{"type", "continental"}}},
                             {"geology",
                              {{"strata", J::array({{{"material", "limestone"}, {"thickness_m", 50}},
                                                    {{"material", "shale"}, {"thickness_m", 50}}})},
                               {"basement_material", "gneiss"},
                               {"strata_top_m", 100},
                               {"folding", {{"enabled", true}, {"axis_angle_deg", 0}, {"wavelength_m", 400},
                                            {"amplitude_m", 50}, {"asymmetry", 0}, {"plunge_deg", 0}}}}}})
                            .island();
    const wg::Geology geology(params);
    const auto name = [&](int id) { return geology.materialNames()[id]; };

    // Fold: amplitude * sin(2 pi v / wavelength), v across the axis (here y).
    CHECK(std::abs(geology.foldOffset(0, 100) - 50.0) < 1e-9);
    CHECK(std::abs(geology.foldOffset(123, 0)) < 1e-9);

    // The strata follow the folds: at the crest (v = 100) they are 50 m higher.
    CHECK(name(geology.material(0, 0, 95)) == "limestone");  // depth 5
    CHECK(name(geology.material(0, 0, 45)) == "shale");      // depth 55
    CHECK(name(geology.material(0, 100, 95)) == "shale");    // crest: depth 55
    CHECK(name(geology.material(0, -100, 45)) == "limestone"); // trough: depth 5
}

// Particle erosion on a small volcanic island (only a few steps).
// `extra`: more top-level config sections.
std::vector<wg::Snapshot> runErosion(nlohmann::json particles, const wg::PipelineCallbacks& callbacks = {},
                                     const nlohmann::json& extra = nlohmann::json::object()) {
    particles["steps"] = particles.value("steps", 30);
    nlohmann::json config = {{"seed", 2},
                              {"world", {{"width", 1024}, {"length", 1024}}},
                              {"simulation", {{"resolution", 256}}},
                              {"origin", {{"type", "volcanic"}}},
                              {"erosion", {{"particles", particles}}}};
    config.update(extra);
    const auto loaded = wg::WorldConfig::fromJson(config);
    return wg::Pipeline::fromConfig(loaded).run(loaded, callbacks);
}

void testErosion() {
    std::vector<std::string> logs;
    wg::PipelineCallbacks callbacks;
    callbacks.onLog = [&](std::size_t, const std::string& message) { logs.push_back(message); };
    const auto snapshots = runErosion(nlohmann::json::object(), callbacks);
    CHECK(snapshots.size() == 6 && snapshots[2].stageName == "erosion" && snapshots[3].stageName == "upscale" &&
          snapshots[4].stageName == "hydrology" && snapshots[5].stageName == "voxelize");
    const auto& state = snapshots[2].state;
    CHECK(snapshots[2].step == 30 && snapshots[2].totalSteps == 30);

    // Something was eroded and deposited, every height is finite.
    const auto [erodedMin, erodedMax] = wg::valueRange(state.layer(wg::layers::kEroded));
    CHECK(erodedMax > 0.1f && erodedMin < -0.1f);
    const auto [dMin, dMax] = wg::valueRange(state.layer(wg::layers::kDischargeNorm));
    CHECK(dMin >= 0.0f && dMax > 0.05f && dMax <= 1.0f); // erf rounds to 1 in float for strong flows
    CHECK(std::all_of(state.layer(wg::layers::kHeight).data().begin(), state.layer(wg::layers::kHeight).data().end(),
                      [](float h) { return std::isfinite(h); }));
    // FIXED border untouched.
    CHECK(state.layer(wg::layers::kHeight).at(0, 7) == snapshots[1].state.layer(wg::layers::kHeight).at(0, 7));

    // Mass balance, logged: Σ h + sediment out of the domain is conserved.
    double drift = 1.0;
    const auto last = std::find_if(logs.rbegin(), logs.rend(), [](const std::string& m) { return m.starts_with("step "); });
    CHECK(last != logs.rend());
    std::sscanf(last->c_str(), "step %*d: %*f ms/step, %*d lakes, sediment out %*f, mass drift %*g (%lg", &drift);
    CHECK(std::abs(drift) < 1e-7);

    // Deterministic.
    const auto again = runErosion(nlohmann::json::object());
    const auto a = state.layer(wg::layers::kHeight).data(), b = again[2].state.layer(wg::layers::kHeight).data();
    CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));

    // Disabled: heights unchanged.
    const auto off = runErosion({{"enabled", false}});
    const auto c = off[2].state.layer(wg::layers::kHeight).data(), d = off[1].state.layer(wg::layers::kHeight).data();
    CHECK(std::equal(c.begin(), c.end(), d.begin(), d.end()));
}

// A small island: rivers from small basins.
const nlohmann::json kSmallRivers = {{"hydrology", {{"river_min_area_m2", 4000}}}};

void testHydrology() {
    const auto snapshots = runErosion(nlohmann::json::object(), {}, kSmallRivers);
    CHECK(snapshots[3].stageName == "upscale" && snapshots[4].stageName == "hydrology");
    const auto& state = snapshots[4].state;
    const auto& info = state.info();
    const auto& h = state.layer(wg::layers::kHeight);
    const auto& water = state.layer(wg::layers::kWaterSurface);
    const auto& kind = state.layer(wg::layers::kWater);
    CHECK(h.width() == info.width && h.length() == info.length); // upscaled to the blocks
    const int nx = h.width(), ny = h.length();
    const auto is = [&](int i, int j, wg::layers::WaterKind k) { return static_cast<int>(kind.at(i, j)) == k; };

    // Every river reaches the sea: flood from the sea through rivers and lakes.
    std::vector<char> reached(static_cast<std::size_t>(nx) * ny, 0);
    std::vector<std::pair<int, int>> stack;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            if (is(i, j, wg::layers::kSea))
                reached[j * nx + i] = 1, stack.push_back({i, j});
    while (!stack.empty()) {
        const auto [i, j] = stack.back();
        stack.pop_back();
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                const int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= nx || nj >= ny || reached[nj * nx + ni] ||
                    !(is(ni, nj, wg::layers::kRiver) || is(ni, nj, wg::layers::kLake)))
                    continue;
                reached[nj * nx + ni] = 1;
                stack.push_back({ni, nj});
            }
        }
    }
    int rivers = 0, cut = 0, narrow = 0, banks = 0, steep = 0;
    bool consistent = true;
    const double cellM = info.widthM() / nx;
    const float rise = static_cast<float>(std::tan(35.0 * std::numbers::pi / 180.0) * cellM) + 1e-3f; // default slope
    for (int j = 1; j < ny - 1; ++j) {
        for (int i = 1; i < nx - 1; ++i) {
            if (!is(i, j, wg::layers::kRiver))
                continue;
            ++rivers;
            cut += !reached[j * nx + i];
            consistent = consistent && water.at(i, j) >= h.at(i, j) + 0.99f; // a bed under the surface
            // A source stretch one block wide.
            narrow += (!is(i - 1, j, wg::layers::kRiver) && !is(i + 1, j, wg::layers::kRiver)) ||
                      (!is(i, j - 1, wg::layers::kRiver) && !is(i, j + 1, wg::layers::kRiver));
            // Banks: no higher than the bank slope from the water.
            for (const auto [di, dj] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                if (!is(i + di, j + dj, wg::layers::kDry))
                    continue;
                ++banks;
                steep += h.at(i + di, j + dj) > water.at(i, j) + rise;
            }
        }
    }
    std::printf("  hydrology: %d river cells, %d of %d banks steeper than the bank slope\n", rivers, steep, banks);
    CHECK(rivers > 100 && cut == 0 && consistent && narrow > 0);
    CHECK(steep * 100 < banks); // a few where a river falls by a step
    // The dry relief is kept: lowered at most by a cut channel (3 m) or the
    // foot of a bank (4 m), never planed down (a steep hillside above a river).
    const auto& before = snapshots[3].state.layer(wg::layers::kHeight);
    float deepestCut = 0.0f;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            if (is(i, j, wg::layers::kDry))
                deepestCut = std::max(deepestCut, before.at(i, j) - h.at(i, j));
    CHECK(deepestCut <= 4.01f);

    // Every lake and river cell holds at least one block of water (top
    // blocks at floor(y) - 1, see the voxelization), and the sea floor in
    // front of a mouth rises at most at the underwater talus (18°).
    int dryWater = 0, mouthSteps = 0;
    const float underwaterRise = static_cast<float>(std::tan(18.0 * std::numbers::pi / 180.0) * cellM) *
                                     std::numbers::sqrt2_v<float> + 1e-3f;
    for (int j = 1; j < ny - 1; ++j) {
        for (int i = 1; i < nx - 1; ++i) {
            if (!is(i, j, wg::layers::kRiver) && !is(i, j, wg::layers::kLake))
                continue;
            dryWater += std::floor(water.at(i, j)) <= std::floor(h.at(i, j));
            if (!is(i, j, wg::layers::kRiver))
                continue;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di)
                    mouthSteps += is(i + di, j + dj, wg::layers::kSea) && h.at(i + di, j + dj) > h.at(i, j) + underwaterRise;
        }
    }
    CHECK(dryWater == 0 && mouthSteps == 0);
}

void testVoxelize() {
    const auto snapshots = runErosion(nlohmann::json::object(), {}, kSmallRivers);
    const auto& state = snapshots[5].state;
    CHECK(snapshots[5].stageName == "voxelize" && state.hasVoxels() && snapshots[5].voxelsChanged);
    const auto& voxels = state.voxels();
    const auto& info = state.info();
    CHECK(voxels.width() == info.width && voxels.length() == info.length);

    // Open sea in a corner: bedrock, stone, then water up to sea level.
    CHECK(voxels.get(3, info.minY, 3) == wg::Block::Bedrock);
    CHECK(voxels.get(3, info.seaLevel - 1, 3) == wg::Block::Water && voxels.get(3, info.seaLevel, 3) == wg::Block::Air);

    // One column per cell of h: on dry land, the top face of the ground is at
    // floor(h) (its top block one below), like the sea's at sea level.
    const auto& h = state.layer(wg::layers::kHeight);
    const auto& kind = state.layer(wg::layers::kWater);
    int counts[static_cast<int>(wg::Block::Count)] = {};
    bool groundMatches = true;
    for (int z = 0; z < info.length; ++z) {
        for (int x = 0; x < info.width; ++x) {
            const int top = voxels.topNonAir(x, z);
            ++counts[static_cast<int>(voxels.get(x, top, z))];
            if (static_cast<int>(kind.at(x, z)) == wg::layers::kDry && h.at(x, z) > info.minY + 1)
                groundMatches = groundMatches && top == static_cast<int>(std::floor(h.at(x, z) / info.blockSizeM)) - 1;
        }
    }
    CHECK(groundMatches);
    CHECK(counts[static_cast<int>(wg::Block::Grass)] > 0 && counts[static_cast<int>(wg::Block::Stone)] > 0);

    // Water above sea level (rivers), and no wall of water: a water block
    // never has air beside it above dry ground (it may above lower water: a
    // cascade).
    int riverWater = 0, walls = 0;
    for (int z = 1; z < info.length - 1; ++z) {
        for (int x = 1; x < info.width - 1; ++x) {
            const int top = voxels.topNonAir(x, z);
            if (voxels.get(x, top, z) != wg::Block::Water)
                continue;
            riverWater += top >= info.seaLevel;
            for (const auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const int ntop = voxels.topNonAir(x + dx, z + dz);
                walls += ntop < top && voxels.get(x + dx, ntop, z + dz) != wg::Block::Water;
            }
        }
    }
    CHECK(riverWater > 0 && walls == 0);
}

void testNbt() {
    // Byte for byte: TAG_Compound "" { TAG_Int "a" = 1, TAG_List "l" of 1 string "x" }.
    wg::nbt::Writer w;
    w.beginCompound("");
    w.intTag("a", 1);
    w.beginList("l", wg::nbt::Tag::String, 1);
    w.stringPayload("x");
    w.endCompound();
    const std::vector<std::uint8_t> expected{10, 0, 0, 3, 0, 1, 'a', 0, 0, 0, 1,
                                             9, 0, 1, 'l', 8, 0, 0, 0, 1, 0, 1, 'x', 0};
    CHECK(w.bytes() == expected);
}

void testMinecraftExport() {
    const auto snapshots = runErosion({{"steps", 5}});
    const auto dir = std::filesystem::temp_directory_path() / "wg_minecraft_test";
    std::filesystem::remove_all(dir);
    const auto report = wg::exportMinecraft(snapshots.back().state, dir);
    // 1024 blocks: 64 x 64 chunks, centered on the origin: regions -1..0 on both axes.
    CHECK(report.chunks == 64 * 64 && report.regions == 4 && report.sectionsDropped == 0);
    CHECK(std::filesystem::file_size(dir / "level.dat") > 100);
    for (const char* region : {"r.-1.-1.mca", "r.-1.0.mca", "r.0.-1.mca", "r.0.0.mca"}) {
        const auto path = dir / "region" / region;
        CHECK(std::filesystem::exists(path) && std::filesystem::file_size(path) % 4096 == 0 &&
              std::filesystem::file_size(path) > 8192);
    }
    CHECK(report.spawnY > snapshots.back().state.info().seaLevel); // on land
    std::filesystem::remove_all(dir);
}

void testAvalanche() {
    // A spike on a flat bare-rock plain collapses to slopes within the talus,
    // without creating or losing matter.
    const auto params = wg::WorldConfig::fromJson({{"seed", 1},
                                                   {"origin", {{"type", "continental"}}},
                                                   {"geology", {{"strata", {{{"material", "granite"}, {"thickness_m", 500}}}},
                                                                {"strata_top_m", 100},
                                                                {"folding", {{"enabled", false}}}}}})
                            .island();
    const wg::Geology geology(params);
    const int n = 15;
    const wg::erosion::Rock rock(geology, params.geology.materials, {n, n, 1.0, 1.0, 0.0}, 1.0, 33.0, 18.0);
    wg::erosion::ErosionMap map(n, n);
    map.at(7, 7).height = 20.0f;
    double before = 0.0;
    for (std::size_t k = 0; k < map.size(); ++k)
        before += map[k].height;
    for (int pass = 0; pass < 200; ++pass)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                wg::erosion::avalanche(map, rock, 1.0f, i, j);
    double after = 0.0, steepest = 0.0;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            steepest = std::max(steepest, static_cast<double>(std::abs(map.at(i, j).height - map.at(i + 1, j).height)));
            after += map.at(i, j).height;
        }
        after += map.at(n - 1, j).height;
    }
    CHECK(std::abs(after - before) < 1e-3);
    // The talus of the higher cell bounds each slope: the bare granite top
    // (50°) stands steeper than the sediment around it (33°).
    CHECK(steepest <= std::tan(50.0 * std::numbers::pi / 180.0) + 0.05);
    CHECK(map.at(7, 7).height < 10.0f && map.at(7, 7).sediment == 0.0f && map.at(7, 9).sediment > 0.0f);
}

void testDropOnFadingFlow() {
    // The averaged flow decays towards denormal floats where nothing flows any
    // more: a drop crossing such cells must stay finite (it used to divide by
    // an underflowed product and spread NaNs into the momentum map).
    const auto params = wg::WorldConfig::fromJson({{"seed", 1}}).island();
    const wg::Geology geology(params);
    const int n = 32;
    const wg::erosion::Rock rock(geology, params.geology.materials, {n, n, 1.0, 1.0, 0.0}, 1.0, 33.0, 18.0);
    wg::erosion::ErosionMap map(n, n);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            auto& cell = map.at(i, j);
            cell.height = 10.0f - 0.0001f * static_cast<float>(i); // nearly flat
            cell.momentumX = 1e-43f, cell.momentumY = -4e-44f, cell.discharge = 1e-43f;
        }
    }
    wg::erosion::DropParams drop;
    drop.rock = &rock;
    drop.thermalRate = 0.8f;
    wg::erosion::descend(map, drop, 5.5f, 16.5f);
    bool finite = true;
    for (std::size_t k = 0; k < map.size(); ++k)
        finite = finite && std::isfinite(map[k].momentumXTrack) && std::isfinite(map[k].momentumYTrack) &&
                 std::isfinite(map[k].height);
    CHECK(finite);
}

void testLakes() {
    // A crater on a hill: a lake up to its rim; drops running into it only
    // deposit there (no erosion under water), as in the sea.
    const auto params = wg::WorldConfig::fromJson({{"seed", 1}}).island();
    const wg::Geology geology(params);
    const int n = 41;
    const wg::erosion::Rock rock(geology, params.geology.materials, {n, n, 1.0, 1.0, 0.0}, 1.0, 33.0, 18.0);
    wg::erosion::ErosionMap map(n, n);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float r = std::hypot(static_cast<float>(i - 20), static_cast<float>(j - 20));
            map.at(i, j).height = r < 8.0f ? 20.0f + 0.5f * r : 24.0f - 0.25f * (r - 8.0f); // rim at r = 8
            if (i == 0 || j == 0 || i == n - 1 || j == n - 1)
                map.setFixed(i, j);
        }
    }
    CHECK(wg::erosion::updateLakes(map, 1.0f) == 1);
    CHECK(map.underwater(20, 20) && std::abs(map.water(20, 20) - map.water(23, 20)) < 1e-6f &&
          map.water(20, 20) > 23.0f && map.water(20, 20) <= 24.0f && !map.underwater(20, 30));
    CHECK(wg::erosion::updateLakes(map, 10.0f) == 0 && !map.underwater(20, 20)); // too shallow for a lake
    wg::erosion::updateLakes(map, 1.0f);
    std::vector<float> before(map.size());
    for (std::size_t k = 0; k < map.size(); ++k)
        before[k] = map[k].height;
    wg::erosion::DropParams drop;
    drop.rock = &rock;
    for (int d = 0; d < 50; ++d) // from just outside the rim, inwards
        wg::erosion::descend(map, drop, 20.5f + 9.0f * std::cos(d * 0.3f), 20.5f + 9.0f * std::sin(d * 0.3f));
    bool erodedUnderwater = false;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            erodedUnderwater |= map.at(i, j).height < map.water(i, j) && map.at(i, j).height < before[map.index(i, j)] - 1e-5f;
    CHECK(!erodedUnderwater);
}

void testStepGate() {
    // A gate asking for every step gets one preview per step, in order.
    std::vector<int> steps;
    wg::PipelineCallbacks callbacks;
    callbacks.previewInterval = 1e9; // no time-based preview
    callbacks.stepGate = [] { return true; };
    callbacks.onStageProgress = [&](std::size_t, wg::Snapshot preview, float) {
        if (preview.stageName == "erosion")
            steps.push_back(preview.step);
    };
    runErosion({{"steps", 5}}, callbacks);
    CHECK((steps == std::vector<int>{1, 2, 3, 4, 5}));
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
    testErosionPresets();
    testReliefByOrigin();
    testReliefZones();
    testAtoll();
    testTopographyByOrigin();
    testGeology();
    testGeologyFolds();
    testErosion();
    testHydrology();
    testVoxelize();
    testNbt();
    testMinecraftExport();
    testAvalanche();
    testDropOnFadingFlow();
    testLakes();
    testStepGate();
    testVoxelVolume();
    testWorldFileRoundTrip();
    testDeterminism();
    if (failures == 0)
        std::puts("all tests passed");
    return failures == 0 ? 0 : 1;
}
