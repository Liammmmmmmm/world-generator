// wmc: converts a generated world (.wgw) into a Minecraft Java Edition world.
//
//   wmc <world.wgw> <output directory> [--name NAME] [--data-version N --version-name 1.21.1]
//
// The output directory is a complete world: copy it into Minecraft's "saves"
// folder and open it.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "wg/io/MinecraftExport.hpp"
#include "wg/io/WorldFile.hpp"

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: wmc <world.wgw> <output directory> [options]\n"
                 "  writes a Minecraft Java Edition world (copy it into .minecraft/saves)\n"
                 "  --name NAME          world name shown in Minecraft (default: the directory name)\n"
                 "  --data-version N     chunk data version (default 3955, Minecraft 1.21.1;\n"
                 "                       newer versions upgrade the world, older ones refuse it)\n"
                 "  --version-name NAME  version name stored in level.dat (default 1.21.1)\n");
}

} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> input, output;
    wg::MinecraftExportOptions options;
    std::optional<std::string> name;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--name" && hasValue)
            name = argv[++i];
        else if (arg == "--data-version" && hasValue)
            options.dataVersion = std::atoi(argv[++i]);
        else if (arg == "--version-name" && hasValue)
            options.versionName = argv[++i];
        else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (!arg.starts_with('-') && !input)
            input = arg;
        else if (!arg.starts_with('-') && !output)
            output = arg;
        else {
            printUsage();
            return 2;
        }
    }
    if (!input || !output || options.dataVersion <= 0) {
        printUsage();
        return 2;
    }
    options.levelName = name.value_or(output->filename().string());

    try {
        std::printf("Reading %s\n", input->string().c_str());
        const auto world = wg::readWorld(*input);
        const auto report = wg::exportMinecraft(world, *output, options, [](float done) {
            std::printf("\r  %3d %%", static_cast<int>(done * 100.0f));
            std::fflush(stdout);
        });
        std::printf("\nMinecraft world written to %s: %d chunks in %d regions, spawn at %d %d %d\n",
                    output->string().c_str(), report.chunks, report.regions, report.spawnX, report.spawnY,
                    report.spawnZ);
        if (report.sectionsDropped > 0)
            std::printf("  %d sections outside the Minecraft overworld (y -64..319) were dropped\n",
                        report.sectionsDropped);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
