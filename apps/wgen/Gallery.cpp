#include "Gallery.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/io/PngExport.hpp"
#include "wg/island/IslandLayers.hpp"
#include "wg/render/ImageOps.hpp"
#include "wg/render/MapRenderer.hpp"

namespace {

constexpr wg::Rgb kBackground{24, 26, 30};
constexpr wg::Rgb kText{230, 230, 230};
constexpr wg::Rgb kDimText{150, 150, 155};
constexpr int kMargin = 10;
constexpr int kCaption = 22;
constexpr int kHeader = 34;

struct Job {
    std::string origin; // empty: as configured / drawn
    int seed = 0;
    wg::RgbaImage thumbnail;
    std::string caption;
    std::string error;
};

void generate(const GalleryOptions& options, const nlohmann::json& raw, Job& job) {
    nlohmann::json config = raw;
    config["seed"] = job.seed;
    if (!job.origin.empty())
        config["origin"]["type"] = job.origin;
    if (options.resolution)
        config["simulation"]["resolution"] = *options.resolution;

    const auto loaded = wg::WorldConfig::fromJson(config);
    const auto snapshots = wg::Pipeline::fromConfig(loaded).run(loaded);
    if (snapshots.empty() || !snapshots.back().state.hasLayer(wg::layers::kHeight))
        throw std::runtime_error("no heightmap produced");
    const auto& state = snapshots.back().state;
    const auto image = wg::renderView(state, {wg::View::Kind::Layer, std::string(wg::layers::kHeight)}, wg::ColorMap::Auto);
    const int height = std::max(1, options.thumbnail * image.height / image.width);
    job.thumbnail = wg::resizeImage(image, options.thumbnail, height);

    const auto& p = state.params().values;
    // With one group per origin, the origin is the group title.
    job.caption = options.origin == "all" ? "seed " + std::to_string(job.seed)
                                          : std::to_string(job.seed) + "  " + p.origin.type;
}

} // namespace

int runGallery(const GalleryOptions& options) {
    nlohmann::json raw;
    try {
        raw = wg::WorldConfig::load(options.configPath).raw;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    // Groups of thumbnails: one per origin with "all", else a single one.
    std::vector<std::string> groups{options.origin.value_or("")};
    if (options.origin == "all")
        groups = {"volcanic", "continental", "sedimentary", "coral"};
    std::vector<Job> jobs;
    for (const auto& origin : groups)
        for (int k = 0; k < options.count; ++k)
            jobs.push_back({origin, options.firstSeed + k, {}, {}, {}});

    // Independent generations, in parallel.
    std::atomic<std::size_t> next{0};
    std::atomic<int> done{0};
    std::mutex printMutex;
    const auto worker = [&] {
        for (std::size_t k; (k = next++) < jobs.size();) {
            try {
                generate(options, raw, jobs[k]);
            } catch (const std::exception& e) {
                jobs[k].error = e.what();
            }
            const std::lock_guard lock(printMutex);
            std::printf("\r  %d / %zu islands", ++done, jobs.size());
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> threads;
    const unsigned threadCount = std::max(1u, std::min(std::thread::hardware_concurrency(), static_cast<unsigned>(jobs.size())));
    for (unsigned t = 0; t < threadCount; ++t)
        threads.emplace_back(worker);
    for (auto& thread : threads)
        thread.join();
    std::printf("\n");

    // Sheet layout: groups stacked, each wrapped in rows of `columns`.
    int tileHeight = 0;
    for (const auto& job : jobs)
        tileHeight = std::max(tileHeight, job.thumbnail.height);
    tileHeight = std::max(tileHeight, options.thumbnail);
    const int columns = std::max(1, std::min(options.columns, options.count));
    const int rowsPerGroup = (options.count + columns - 1) / columns;
    const int cellW = options.thumbnail + kMargin, cellH = tileHeight + kCaption + kMargin;
    const int header = groups.size() > 1 ? kHeader : 0;
    const int sheetW = kMargin + columns * cellW;
    const int sheetH = kMargin + static_cast<int>(groups.size()) * (header + rowsPerGroup * cellH);
    wg::RgbaImage sheet = wg::makeRgbaImage(sheetW, sheetH, kBackground);

    int errors = 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const int groupY = kMargin + static_cast<int>(g) * (header + rowsPerGroup * cellH);
        if (header)
            wg::drawText(sheet, kMargin, groupY + 6, groups[g], kText, 3);
        for (int k = 0; k < options.count; ++k) {
            const Job& job = jobs[g * options.count + k];
            const int x = kMargin + (k % columns) * cellW;
            const int y = groupY + header + (k / columns) * cellH;
            if (!job.error.empty()) {
                ++errors;
                wg::drawText(sheet, x, y + 4, "seed " + std::to_string(job.seed) + ": error", {255, 100, 90}, 2);
                std::fprintf(stderr, "seed %d: %s\n", job.seed, job.error.c_str());
                continue;
            }
            wg::blit(sheet, job.thumbnail, x, y);
            std::string caption = job.caption; // shortened to the thumbnail width
            while (caption.size() > 1 && wg::textWidth(caption, 2) > options.thumbnail)
                caption.pop_back();
            wg::drawText(sheet, x, y + tileHeight + 5, caption, kDimText, 2);
        }
    }

    try {
        wg::writePng(sheet, options.outputPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("Gallery of %zu islands saved to %s\n", jobs.size() - errors, options.outputPath.string().c_str());
    return errors == 0 ? 0 : 1;
}
