// wgen: generates a world from a JSON config and saves it.
//
//   wgen <config.json> [-o <world.wgw>] [--png <map.png>] [--stages <dir>]

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/io/PngExport.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/render/MapRenderer.hpp"

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: wgen <config.json> [-o <world.wgw>] [--png <map.png>] [--stages <dir>]\n"
                 "  -o        output world file (default: output/<config name>.wgw)\n"
                 "  --png     export a top-down map of the final world\n"
                 "  --stages  export a top-down map after every stage into <dir>\n");
}

} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> configPath, outPath, pngPath, stagesDir;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if ((arg == "-o" || arg == "--output") && hasValue)
            outPath = argv[++i];
        else if (arg == "--png" && hasValue)
            pngPath = argv[++i];
        else if (arg == "--stages" && hasValue)
            stagesDir = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (!arg.starts_with('-') && !configPath)
            configPath = arg;
        else {
            printUsage();
            return 2;
        }
    }
    if (!configPath) {
        printUsage();
        return 2;
    }
    if (!outPath)
        outPath = fs::path("output") / configPath->stem().concat(".wgw");

    try {
        const auto config = wg::WorldConfig::load(*configPath);
        const auto pipeline = wg::Pipeline::fromConfig(config);
        std::printf("Generating %dx%d world (seed %llu, %zu stages)\n", config.width, config.length,
                    static_cast<unsigned long long>(config.seed), pipeline.size());

        wg::PipelineCallbacks callbacks;
        callbacks.onStageDone = [&](std::size_t index, const wg::Snapshot& snapshot) {
            std::printf("  %2zu. %-16s %-12s %8.1f ms\n", index + 1, snapshot.stageName.c_str(),
                        snapshot.stageType.c_str(), snapshot.seconds * 1000.0);
            if (stagesDir) {
                char fileName[256];
                std::snprintf(fileName, sizeof fileName, "%02zu_%s.png", index + 1, snapshot.stageName.c_str());
                if (const auto view = wg::defaultView(snapshot); wg::hasView(snapshot.state, view))
                    wg::writePng(wg::renderView(snapshot.state, view, wg::ColorMap::Terrain), *stagesDir / fileName);
            }
        };
        const auto snapshots = pipeline.run(config.info(), callbacks);
        if (snapshots.empty())
            throw std::runtime_error("the pipeline is empty, nothing to generate");
        const wg::Snapshot& final = snapshots.back();

        wg::writeWorld(final.state, *outPath);
        std::printf("World saved to %s\n", outPath->string().c_str());

        if (pngPath) {
            const auto view = wg::defaultView(final);
            if (!wg::hasView(final.state, view))
                throw std::runtime_error("the final world has nothing to render");
            wg::writePng(wg::renderView(final.state, view, wg::ColorMap::Terrain), *pngPath);
            std::printf("Map saved to %s\n", pngPath->string().c_str());
        }
        if (stagesDir)
            std::printf("Stage maps saved to %s\n", stagesDir->string().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
