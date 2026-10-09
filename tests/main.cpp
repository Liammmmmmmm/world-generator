// Minimal self-contained tests (no framework yet). Run: ctest --test-dir build

#include <cstdio>
#include <filesystem>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/gen/StageRegistry.hpp"
#include "wg/io/WorldFile.hpp"

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
        r.add("flat", [](wg::StageParams& p) { return wg::StageRegistry::builtin().create("flat", p); });
        r.add<AddStage>("add");
        r.add<StoneStage>("stone");
        return r;
    }();
    return registry;
}

wg::WorldConfig makeConfig(nlohmann::json pipeline, int width = 16, int length = 16) {
    return wg::WorldConfig::fromJson({{"seed", 1}, {"width", width}, {"length", length}, {"pipeline", pipeline}});
}

wg::Pipeline makePipeline(const wg::WorldConfig& config) {
    return wg::Pipeline::fromConfig(config, testRegistry());
}

// --- Tests -------------------------------------------------------------------------

void testConfig() {
    const auto config = wg::WorldConfig::fromJson({{"seed", 42}, {"width", 64}, {"length", 32}});
    CHECK(config.seed == 42 && config.width == 64 && config.length == 32);

    CHECK(wg::WorldConfig::fromJson({{"seed", "abc"}, {"width", 1}, {"length", 1}}).seed == wg::hashString("abc"));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"width", 1}, {"length", 1}}); }));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"seed", 1}, {"width", 0}, {"length", 1}}); }));
    CHECK(throws<wg::ConfigError>([] { wg::WorldConfig::fromJson({{"seed", 1}, {"width", 1.5}, {"length", 1}}); }));
}

void testPipelineConfig() {
    using J = nlohmann::json;
    const auto build = [](J pipeline) { makePipeline(makeConfig(pipeline)); };
    CHECK(!throws<wg::ConfigError>([&] { build(J::array()); }));
    CHECK(!throws<wg::ConfigError>([&] { build(J::array({{{"type", "flat"}}, {{"type", "add"}}})); }));
    CHECK(throws<wg::ConfigError>([&] { wg::Pipeline::fromConfig(wg::WorldConfig::fromJson({{"seed", 1}, {"width", 1}, {"length", 1}})); }));
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
    const auto snapshots = makePipeline(config).run(config.info());
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
    CHECK(throws<wg::GenerationError>([&] { makePipeline(config).run(config.info()); }));
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
    pipeline.run(config.info(), callbacks);

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
    CHECK(throws<wg::GenerationCancelled>([&] { pipeline.run(config.info(), cancelling); }));
    CHECK(reports == 6);
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
    wg::WorldState world(wg::WorldInfo{37, 21, -64, 320, 62, 1});
    world.createLayer("height", 3.5f).at(36, 20) = 12.0f;
    world.createLayer("moisture", 0.25f);
    auto& voxels = world.createVoxels();
    voxels.fillBox(0, -64, 0, 37, 40, 21, wg::Block::Stone);
    voxels.set(4, 100, 2, wg::Block::Snow);

    const auto path = std::filesystem::temp_directory_path() / "wg_roundtrip.wgw";
    wg::writeWorld(world, path);
    const auto loaded = wg::readWorld(path);
    std::filesystem::remove(path);

    CHECK(loaded.info().width == 37 && loaded.info().length == 21 && loaded.info().seed == 1);
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
    testPipelineConfig();
    testSnapshots();
    testMissingLayerFails();
    testLiveCallbacks();
    testVoxelVolume();
    testWorldFileRoundTrip();
    testDeterminism();
    if (failures == 0)
        std::puts("all tests passed");
    return failures == 0 ? 0 : 1;
}
