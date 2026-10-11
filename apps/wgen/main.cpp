// wgen: generates a world from a JSON config and saves it.
//
//   wgen <config.jsonc> [-o <world.wgw>] [--params <params.json>] [--png <map.png>] [--stages <dir>]
//   wgen <config.jsonc> --gallery <sheet.png> [--count N] [--origin <type|all>] [--first-seed S]
//                       [--resolution N] [--columns N] [--thumbnail PX]

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "Calibrate.hpp"
#include "Gallery.hpp"
#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/io/PngExport.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/render/MapRenderer.hpp"

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: wgen <config.jsonc> [-o <world.wgw>] [--params <params.json>] [--png <map.png>] [--stages <dir>]\n"
                 "  -o        output world file (default: output/<config name>.wgw)\n"
                 "  --params  write the resolved parameters (every value filled in) as JSON\n"
                 "  --png     export a top-down map of the final world\n"
                 "  --stages  export a top-down map after every stage into <dir>\n"
                 "\n"
                 "usage: wgen <config.jsonc> --gallery <sheet.png> [options]\n"
                 "  generates many seeds and assembles hillshaded thumbnails into one sheet\n"
                 "  --count N        seeds to generate, per origin with --origin all (default 16)\n"
                 "  --origin TYPE    force origin.type (volcanic, continental, sedimentary, coral),\n"
                 "                   or \"all\" for one group per origin\n"
                 "  --first-seed S   first seed (default 1)\n"
                 "  --resolution N   override simulation.coarse_resolution (e.g. 256 for speed)\n"
                 "  --columns N      thumbnails per row (default 8)\n"
                 "  --thumbnail PX   thumbnail width (default 256)\n"
                 "\n"
                 "usage: wgen <config.jsonc> --calibrate [--resolution N]\n"
                 "  steady-state relief of a fixed island for a grid of uplift rates and k_base\n"
                 "  (only world.* is read from the config; --resolution defaults to 512)\n");
}

} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> configPath, outPath, paramsPath, pngPath, stagesDir;
    std::optional<fs::path> galleryPath;
    bool calibrate = false;
    GalleryOptions gallery;
    const auto number = [](const char* text) { // -1 when not a number (then rejected)
        try {
            return std::stoi(text);
        } catch (const std::exception&) {
            return -1;
        }
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if ((arg == "-o" || arg == "--output") && hasValue)
            outPath = argv[++i];
        else if (arg == "--params" && hasValue)
            paramsPath = argv[++i];
        else if (arg == "--png" && hasValue)
            pngPath = argv[++i];
        else if (arg == "--stages" && hasValue)
            stagesDir = argv[++i];
        else if (arg == "--calibrate")
            calibrate = true;
        else if (arg == "--gallery" && hasValue)
            galleryPath = argv[++i];
        else if (arg == "--count" && hasValue)
            gallery.count = number(argv[++i]);
        else if (arg == "--origin" && hasValue)
            gallery.origin = argv[++i];
        else if (arg == "--first-seed" && hasValue)
            gallery.firstSeed = number(argv[++i]);
        else if (arg == "--resolution" && hasValue)
            gallery.resolution = number(argv[++i]);
        else if (arg == "--columns" && hasValue)
            gallery.columns = number(argv[++i]);
        else if (arg == "--thumbnail" && hasValue)
            gallery.thumbnail = number(argv[++i]);
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
    if (calibrate) {
        CalibrateOptions options;
        options.configPath = *configPath;
        if (gallery.resolution) {
            if (*gallery.resolution < 8) {
                printUsage();
                return 2;
            }
            options.resolution = *gallery.resolution;
        }
        return runCalibration(options);
    }
    if (galleryPath) {
        gallery.configPath = *configPath;
        gallery.outputPath = *galleryPath;
        if (gallery.count < 1 || gallery.columns < 1 || gallery.thumbnail < 16 || gallery.firstSeed < 0 ||
            (gallery.resolution && *gallery.resolution < 8)) {
            printUsage();
            return 2;
        }
        return runGallery(gallery);
    }
    if (!outPath)
        outPath = fs::path("output") / configPath->stem().concat(".wgw");

    try {
        const auto config = wg::WorldConfig::load(*configPath);
        const auto pipeline = wg::Pipeline::fromConfig(config);
        const auto info = config.info();
        std::printf("Generating %dx%d world (seed %llu, %zu stages)\n", info.width, info.length,
                    static_cast<unsigned long long>(config.seed), pipeline.size());

        int counts[3] = {};
        for (const auto& entry : config.params->entries)
            ++counts[static_cast<int>(entry.source)];
        std::printf("Parameters: %d drawn from the seed, %d from the config, %d defaults\n", counts[0], counts[1],
                    counts[2]);
        if (paramsPath) {
            if (paramsPath->has_parent_path())
                fs::create_directories(paramsPath->parent_path());
            std::ofstream(*paramsPath) << config.params->toJson().dump(2) << '\n';
            std::printf("Resolved parameters saved to %s\n", paramsPath->string().c_str());
        }

        wg::PipelineCallbacks callbacks;
        callbacks.onLog = [](std::size_t index, const std::string& message) {
            std::printf("      [%zu] %s\n", index + 1, message.c_str());
            std::fflush(stdout);
        };
        callbacks.onStageDone = [&](std::size_t index, const wg::Snapshot& snapshot) {
            std::printf("  %2zu. %-16s %-12s %8.1f ms\n", index + 1, snapshot.stageName.c_str(),
                        snapshot.stageType.c_str(), snapshot.seconds * 1000.0);
            if (stagesDir) {
                char fileName[256];
                std::snprintf(fileName, sizeof fileName, "%02zu_%s.png", index + 1, snapshot.stageName.c_str());
                if (const auto view = wg::defaultView(snapshot); view.isImage() && wg::hasView(snapshot.state, view))
                    wg::writePng(wg::renderView(snapshot.state, view, wg::ColorMap::Auto), *stagesDir / fileName);
            }
        };
        const auto snapshots = pipeline.run(config, callbacks);
        if (snapshots.empty())
            throw std::runtime_error("the pipeline is empty, nothing to generate");
        const wg::Snapshot& final = snapshots.back();

        wg::writeWorld(final.state, *outPath);
        std::printf("World saved to %s\n", outPath->string().c_str());

        if (pngPath) {
            auto view = wg::defaultView(final);
            if (!view.isImage()) { // fall back to the last map-like view
                for (const auto& v : wg::availableViews(final.state))
                    if (v.isImage())
                        view = v;
            }
            if (view.isImage() && wg::hasView(final.state, view)) {
                wg::writePng(wg::renderView(final.state, view, wg::ColorMap::Auto), *pngPath);
                std::printf("Map saved to %s\n", pngPath->string().c_str());
            } else {
                std::printf("No map to save yet (no layer or voxels in the final world)\n");
            }
        }
        if (stagesDir)
            std::printf("Stage maps saved to %s\n", stagesDir->string().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
