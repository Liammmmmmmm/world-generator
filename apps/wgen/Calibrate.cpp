#include "Calibrate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/island/IslandLayers.hpp"

namespace {

using J = nlohmann::json;

constexpr double kUpliftRates[] = {1e-4, 5e-4, 1e-3};       // m/yr
constexpr double kErodibilities[] = {1e-6, 3e-6, 1e-5, 3e-5}; // k_base
constexpr double kMaxYears = 3e7;
constexpr double kDtYears = 2e4;
constexpr double kEquilibrium = 0.01; // mean |dh/dt| below this share of U

// The calibration island: seed and shape imposed, one rock of erodibility 1,
// one dome at a constant rate, rivers only (no deposition, no hillslopes, no
// sea level changes), uniform rainfall of 1 m/yr.
J calibrationConfig(const J& world, int resolution, double uplift, double kBase) {
    return {
        {"seed", 7},
        {"world", world},
        {"origin", {{"type", "continental"}, {"age", 0.5}}},
        {"shape", {{"lobe_count", 1}, {"lobe_spread", 0.0}, {"radius_ratio", 0.6}, {"elongation", 1.3},
                   {"bay_count", 0}}},
        {"initial_topography", {{"mode", "peneplain"}, {"base_elevation_m", 10}}},
        {"geology",
         {{"materials", {{"calibration_rock", {{"hardness", 0.5}, {"erodibility", 1.0}, {"talus_angle_deg", 45},
                                               {"sediment", "sand"}}}}},
          {"strata", J::array()},
          {"basement_material", "calibration_rock"},
          {"folding", {{"enabled", false}}},
          {"fault_count", 0},
          {"intrusion_count", 0}}},
        {"tectonics",
         {{"uplift_warp_amplitude", 0.0},
          {"uplift_modulation", 0.0},
          {"phases", J::array({{{"type", "dome"}, {"center", {0, 0}}, {"width_m", 1500}, {"rate_m_per_year", uplift},
                                {"start_years", -1e9}, {"end_years", 1e9}}})}}},
        {"climate", {{"base_precipitation_m_per_year", 1.0}}},
        {"timeline", {{"glaciation_count", 0}}},
        {"erosion", {{"fluvial", {{"k_base", kBase}, {"deposition_g", 0.0}}}}},
        {"simulation",
         {{"coarse_resolution", resolution},
          {"total_duration_years", kMaxYears},
          {"dt_years", kDtYears},
          {"log_interval_steps", 10},
          {"processes", {{"hillslope", false}, {"marine", false}}}}},
    };
}

struct Result {
    double maxH = 0.0, p99H = 0.0, meanH = 0.0; // p99: the peaks without the single-cell spikes
    double years = -1.0; // time to equilibrium, < 0 if not reached
};

Result calibrate(const J& world, int resolution, double uplift, double kBase) {
    const auto config = wg::WorldConfig::fromJson(calibrationConfig(world, resolution, uplift, kBase));
    Result result;
    bool done = false;
    double time = 0.0;
    wg::WorldState last;
    wg::PipelineCallbacks callbacks;
    callbacks.previewInterval = 0.0;
    callbacks.onStageProgress = [&](std::size_t, wg::Snapshot preview, float) { last = std::move(preview.state); };
    callbacks.onLog = [&](std::size_t, const std::string& line) {
        const auto t = line.find("t = "), rate = line.find("mean |dh/dt| ");
        if (t == std::string::npos || rate == std::string::npos)
            return;
        const double value = std::stod(line.substr(t + 4));
        time = value * (line.find(" Ma", t) < line.find(" ka", t) ? 1e6 : 1e3);
        if (time >= 1e6 && std::stod(line.substr(rate + 13)) < kEquilibrium * uplift) {
            result.years = time;
            done = true;
        }
    };
    callbacks.shouldCancel = [&] { return done; };
    try {
        last = wg::Pipeline::fromConfig(config).run(config, callbacks).back().state;
    } catch (const wg::GenerationCancelled&) {
    }
    const auto& h = last.layer(wg::layers::kHeight);
    const double sea = last.info().seaLevelM();
    double sum = 0.0;
    std::vector<float> land;
    for (float v : h.data()) {
        result.maxH = std::max(result.maxH, v - sea);
        if (v > sea) {
            sum += v - sea;
            land.push_back(static_cast<float>(v - sea));
        }
    }
    if (!land.empty()) {
        const auto at = land.begin() + static_cast<std::ptrdiff_t>(0.99 * (land.size() - 1));
        std::nth_element(land.begin(), at, land.end());
        result.p99H = *at;
        result.meanH = sum / land.size();
    }
    return result;
}

} // namespace

int runCalibration(const CalibrateOptions& options) {
    try {
        const auto base = wg::WorldConfig::load(options.configPath).raw;
        const J world = base.contains("world") ? base["world"] : J::object();
        const auto probe = wg::WorldConfig::fromJson(calibrationConfig(world, options.resolution, 1e-4, 1e-6));
        const auto& p = probe.island();
        std::printf("Calibration: island R = %.0f m, %d^2 cells of %.1f m, dome sigma 1500 m, P = 1 m/yr, m = %.2f\n",
                    p.islandRadiusM(), options.resolution, p.world.width * p.world.blockSizeM / options.resolution,
                    p.erosion.fluvial.m);
        std::printf("Steady state: mean |dh/dt| on land < %.0f %% of U (checked every 10 steps of %.0f ka, after "
                    "1 Ma), at most %.0f Ma\n\n",
                    100 * kEquilibrium, kDtYears / 1e3, kMaxYears / 1e6);
        std::printf("| U (m/yr) | k_base | U / K | max h (m) | h 99 %% (m) | mean h (m) | equilibrium |\n");
        std::printf("|---|---|---|---|---|---|---|\n");
        std::fflush(stdout);
        for (const double uplift : kUpliftRates) {
            for (const double kBase : kErodibilities) {
                const auto start = std::chrono::steady_clock::now();
                const Result r = calibrate(world, options.resolution, uplift, kBase);
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                char equilibrium[64];
                if (r.years > 0)
                    std::snprintf(equilibrium, sizeof equilibrium, "%.1f Ma", r.years / 1e6);
                else
                    std::snprintf(equilibrium, sizeof equilibrium, "> %.0f Ma", kMaxYears / 1e6);
                std::printf("| %.0e | %.0e | %.0f | %.0f | %.0f | %.0f | %s |   (%.0f s)\n", uplift, kBase, uplift / kBase,
                            r.maxH, r.p99H, r.meanH, equilibrium, seconds);
                std::fflush(stdout);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
