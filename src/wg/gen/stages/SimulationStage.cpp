#include "wg/gen/stages/SimulationStage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include "wg/core/Parallel.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/island/Geology.hpp"
#include "wg/island/IslandGrid.hpp"
#include "wg/island/IslandLayers.hpp"
#include "wg/sim/Climate.hpp"
#include "wg/sim/Coastal.hpp"
#include "wg/sim/Fluvial.hpp"
#include "wg/sim/Glacier.hpp"
#include "wg/sim/Hillslope.hpp"
#include "wg/sim/Marine.hpp"
#include "wg/sim/Routing.hpp"
#include "wg/sim/TimeParams.hpp"

namespace wg {

namespace {

// Below this thickness the sediment cover is not shown as a dominant kind.
constexpr double kVisibleSedimentM = 1e-3;

std::string formatYears(double years) {
    char text[64];
    if (years >= 1e6)
        std::snprintf(text, sizeof text, "%.2f Ma", years / 1e6);
    else
        std::snprintf(text, sizeof text, "%.1f ka", years / 1e3);
    return text;
}

} // namespace

void SimulationStage::run(WorldState& world, const StageContext& context) {
    const IslandParams& p = world.params().values;
    const IslandGrid grid(p);
    const auto& sim = p.simulation;
    const auto& processes = sim.processes;
    const int n = grid.nx * grid.ny;
    const double dx = grid.cellM;

    // --- State (doubles: the routing epsilon is below float precision) -------
    const auto& flagsLayer = world.byteLayer(layers::kFlags);
    std::vector<std::uint8_t> fixed(n);
    for (int c = 0; c < n; ++c)
        fixed[c] = flagsLayer.data()[c] & layers::kFixed;
    std::vector<double> b(n), sed(n), h(n), hStart(n), cumUplift(n), strataUplift(n), discharge(n), lakeDepth(n),
        erosionRate(n), source(n), depositionRate(n), sedimentFlux(n);
    {
        const auto& hL = world.layer(layers::kHeight);
        const auto& sedL = world.layer(layers::kSediment);
        const auto& upL = world.layer(layers::kCumUplift);
        for (int c = 0; c < n; ++c) {
            sed[c] = sedL.data()[c];
            b[c] = hL.data()[c] - sed[c]; // h = b + sed
            cumUplift[c] = upL.data()[c];
        }
    }
    const std::vector<double> fixedHeights = b;

    // Sediment composition (fractions of gravel, sand, clay).
    std::vector<Composition> frac(n);
    {
        const auto& gravel = world.layer(layers::kSedimentGravel);
        const auto& sand = world.layer(layers::kSedimentSand);
        const auto& clay = world.layer(layers::kSedimentClay);
        for (int c = 0; c < n; ++c)
            frac[c] = {gravel.data()[c], sand.data()[c], clay.data()[c]};
    }

    // Rock at the top of the bedrock b of each cell (not at h: under the
    // sediment), re-evaluated every step as b moves: its erodibility, repose
    // angle and the sediment kind it breaks into.
    Geology geology(p);
    struct RockProperties {
        double erodibility, talusTan, hardness;
        int kind;
    };
    std::vector<RockProperties> rocks;
    for (const auto& name : geology.materialNames()) {
        const auto& material = p.geology.materials.at(name);
        rocks.push_back({material.erodibility, std::tan(material.talusAngleDeg * std::numbers::pi / 180.0),
                         material.hardness, material.sediment == "gravel" ? kGravel : material.sediment == "sand" ? kSand : kClay});
    }
    std::vector<int> material(n, 0), rockKind(n, 0);
    std::vector<double> rockTan(n, 1.0), rockHardness(n, 0.5);
    std::vector<float> fold(n), zRef(n);
    {
        const auto& foldL = world.layer(layers::kFoldOffset);
        const auto& initial = world.layer(layers::kHeightInitial);
        // z_ref is the original surface for volcanic strata, else the top of the pile
        // (the lookup removes the uplift from z).
        const double strataTop = world.info().seaLevelM() + p.geology.strataTopM;
        for (int c = 0; c < n; ++c) {
            fold[c] = foldL.data()[c];
            zRef[c] = p.origin.type == "volcanic" ? initial.data()[c] : static_cast<float>(strataTop);
        }
    }
    const Regolith regolith{b, sed, frac, rockKind};

    // Tectonic phases: pattern, and whether their strata move with faults
    // (block_uplift / horst_graben: Geology moves them, so not cum_uplift).
    const auto& phases = p.tectonics.phases;
    std::vector<const Grid2D<float>*> patterns;
    std::vector<bool> faulted;
    for (std::size_t k = 0; k < phases.size(); ++k) {
        patterns.push_back(&world.layer(layers::upliftPattern(static_cast<int>(k))));
        faulted.push_back(phases[k].type == "block_uplift" || phases[k].type == "horst_graben");
    }

    // --- Output layers ----------------------------------------------------------
    world.createLayer(layers::kBedrock, grid.nx, grid.ny, 0.0f, {LayerStyle::Height, {}});
    world.createLayer(layers::kDischarge, grid.nx, grid.ny, 0.0f, {LayerStyle::LogScalar, {}});
    world.createLayer(layers::kLakeDepth, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    world.createLayer(layers::kErosionRate, grid.nx, grid.ny, 0.0f, {LayerStyle::Diverging, {}});
    world.createLayer(layers::kStrataUplift, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    world.createLayer(layers::kSedimentFlux, grid.nx, grid.ny, 0.0f, {LayerStyle::LogScalar, {}});
    world.createLayer(layers::kDepositionRate, grid.nx, grid.ny, 0.0f, {LayerStyle::LogScalar, {}});
    world.createLayer(layers::kTemperature, grid.nx, grid.ny, 0.0f, {LayerStyle::Diverging, {}});
    world.createLayer(layers::kSlidingVelocity, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    world.createLayer(layers::kMassBalance, grid.nx, grid.ny, 0.0f, {LayerStyle::Diverging, {}});
    world.createLayer(layers::kGlacialErosion, grid.nx, grid.ny, 0.0f, {LayerStyle::LogScalar, {}});
    world.createLayer(layers::kPrecipitation, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    world.createLayer(layers::kCoastExposure, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});
    world.createLayer(layers::kCoastAccumulator, grid.nx, grid.ny, 0.0f, {LayerStyle::Scalar, {}});

    // --- Time ------------------------------------------------------------------
    const double duration = sim.totalDurationYears;
    const double dt = sim.dtYears;
    const int steps = std::max(1, static_cast<int>(std::ceil(duration / dt - 1e-9)));
    std::vector<double> phaseDone(phases.size(), 0.0), phaseTotal(phases.size(), 0.0);
    const auto stepSpan = [&](int step, double& t0, double& length) {
        t0 = step * dt;
        length = std::min(dt, duration - t0);
    };
    for (int step = 0; step < steps; ++step) {
        double t0, length;
        stepSpan(step, t0, length);
        for (std::size_t k = 0; k < phases.size(); ++k)
            phaseTotal[k] += phaseActivity(phases[k], t0 + length / 2) * length;
    }
    double time = 0.0;
    int stepsDone = 0;

    // Strata moved by the non-faulted uplift and by the faulted blocks, as far
    // as their phases went.
    const auto updateMaterials = [&] {
        std::vector<double> progress(phases.size());
        for (std::size_t k = 0; k < phases.size(); ++k)
            progress[k] = phaseTotal[k] > 0 ? phaseDone[k] / phaseTotal[k] : 0.0;
        geology.setPhaseProgress(progress);
        parallelFor(grid.ny, [&](int j0, int j1) {
            for (int j = j0; j < j1; ++j) {
                for (int i = 0; i < grid.nx; ++i) {
                    const int c = j * grid.nx + i;
                    material[c] = geology.material(grid.x(i), grid.y(j), b[c], strataUplift[c], zRef[c], fold[c]);
                    rockKind[c] = rocks[material[c]].kind;
                    rockTan[c] = rocks[material[c]].talusTan;
                    rockHardness[c] = rocks[material[c]].hardness;
                }
            }
        });
    };

    updateMaterials();

    // Climate and coasts.
    ClimateModel climateModel(p, grid.nx, grid.ny, dx);
    std::vector<double> surface(n), temperature(n, p.climate.baseTemperatureC),
        precipitation(n, p.climate.basePrecipitationMPerYear), coastWear(n), ice(n), sliding(n, 0.0),
        massBalance(n, 0.0), glacialErosion(n, 0.0);
    {
        const auto& iceL = world.layer(layers::kIce);
        for (int c = 0; c < n; ++c)
            ice[c] = iceL.data()[c];
    }
    CoastalErosion coastal(grid.nx, grid.ny);
    CoastalSettings coastSettings;
    {
        const auto& co = p.erosion.coastal;
        coastSettings.waveErosionCoeff = co.waveErosionCoeff * p.multipliers().marine;
        coastSettings.swellEnergy = p.climate.swell.energy;
        coastSettings.swellDirectionDeg = p.climate.swell.directionDeg;
        coastSettings.fetchRayCount = co.fetchRayCount;
        coastSettings.fetchMaxM = co.fetchMaxDistanceM;
        coastSettings.platformDepthM = co.platformDepthM;
        coastSettings.platformAttenuationM = co.platformAttenuationM;
        coastSettings.exportThreshold = co.sedimentExportThreshold;
        coastSettings.sedimentHardness = co.sedimentHardness;
        coastSettings.cellM = dx;
    }
    double coastDestroyed = 0.0; // m3 since the last log
    double currentSea = world.info().seaLevelM(); // sea level of the current step

    const auto syncLayers = [&] {
        const auto write = [&](std::string_view name, const std::vector<double>& values) {
            auto& layer = world.editLayer(name);
            for (int c = 0; c < n; ++c)
                layer.data()[c] = static_cast<float>(values[c]);
        };
        write(layers::kHeight, h);
        write(layers::kBedrock, b);
        write(layers::kCumUplift, cumUplift);
        write(layers::kStrataUplift, strataUplift);
        write(layers::kDischarge, discharge);
        write(layers::kLakeDepth, lakeDepth);
        write(layers::kErosionRate, erosionRate);
        write(layers::kSediment, sed);
        write(layers::kSedimentFlux, sedimentFlux);
        write(layers::kDepositionRate, depositionRate);
        write(layers::kTemperature, temperature);
        write(layers::kIce, ice);
        write(layers::kSlidingVelocity, sliding);
        write(layers::kMassBalance, massBalance);
        write(layers::kGlacialErosion, glacialErosion);
        write(layers::kPrecipitation, precipitation);
        write(layers::kCoastExposure, coastal.exposure());
        coastal.normalizedAccumulator(h, coastSettings, coastWear);
        write(layers::kCoastAccumulator, coastWear);
        world.setScalar("sea_level_m", currentSea - world.info().seaLevelM());
        auto& gravel = world.editLayer(layers::kSedimentGravel);
        auto& sand = world.editLayer(layers::kSedimentSand);
        auto& clay = world.editLayer(layers::kSedimentClay);
        auto& dominant = world.editLayer(layers::kSedimentDominant);
        auto& surface = world.editLayer(layers::kSurfaceMaterial);
        for (int c = 0; c < n; ++c) {
            gravel.data()[c] = static_cast<float>(frac[c][kGravel]);
            sand.data()[c] = static_cast<float>(frac[c][kSand]);
            clay.data()[c] = static_cast<float>(frac[c][kClay]);
            const auto top = std::max_element(frac[c].begin(), frac[c].end());
            dominant.data()[c] = sed[c] > kVisibleSedimentM ? static_cast<float>(1 + (top - frac[c].begin())) : 0.0f;
            surface.data()[c] = static_cast<float>(material[c]);
        }
        world.setScalar("time_years", time);
        world.setScalar("step", stepsDone);
        world.setScalar("steps", steps);
        for (std::size_t k = 0; k < phases.size(); ++k)
            world.setScalar("phase_progress_" + std::to_string(k), phaseTotal[k] > 0 ? phaseDone[k] / phaseTotal[k] : 0.0);
    };

    // --- Processes ---------------------------------------------------------------
    if (processes.fluvial && p.erosion.fluvial.n != 1.0)
        context.log("warning: erosion.fluvial.n = " + std::to_string(p.erosion.fluvial.n) +
                    " is not supported by the implicit scheme, using n = 1");
    context.log("simulating " + formatYears(duration) + " in " + std::to_string(steps) + " steps of " +
                formatYears(dt) + " on " + std::to_string(grid.nx) + "x" + std::to_string(grid.ny));

    // Coefficients of the processes, times the multipliers of the island's origin.
    const auto& multiply = p.multipliers();
    const double kBase = p.erosion.fluvial.kBase * multiply.fluvial;
    const double cellArea = dx * dx;
    FluvialSettings fluvial;
    fluvial.kSediment = kBase * p.erosion.fluvial.kSedimentMult;
    fluvial.m = p.erosion.fluvial.m;
    fluvial.g = p.erosion.fluvial.depositionG;
    fluvial.cellM = dx;
    fluvial.tolerance = p.erosion.fluvial.depositionTolerance;
    fluvial.maxIterations = p.erosion.fluvial.depositionMaxIterations;
    MarineSettings marine;
    marine.sandD0 = p.erosion.marineDiffusion.sandD0 * multiply.marineDiffusion;
    marine.finesD0 = p.erosion.marineDiffusion.finesD0 * multiply.marineDiffusion;
    marine.depthScaleM = p.erosion.marineDiffusion.depthScaleM;
    marine.mobileLayerM = p.erosion.marineDiffusion.mobileLayerM;
    marine.cellM = dx;
    std::vector<double> drainageArea(n), rockK(n);
    const std::vector<double> cellAreas(n, cellArea);
    std::vector<std::uint8_t> inSea(n);
    // Budget since the last log (m3), and Gauss-Seidel iterations.
    double budgetRock = 0.0, budgetStored = 0.0, budgetExported = 0.0, budgetDrift = 0.0;
    double totalRock = 0.0, totalDrift = 0.0;
    int depositionIterations = 0;
    const double diffusivity = p.erosion.hillslope.diffusivityM2PerYear * multiply.hillslope;
    const auto& thermal = p.erosion.thermal;
    const double thermalRate = std::min(1.0, thermal.rate * multiply.thermal);
    // Glaciers.
    const auto& glacial = p.erosion.glacial;
    GlacierModel glacier(p, grid.nx, grid.ny, dx);
    std::vector<double> iceMelt(n, 0.0), glacialEroded(n, 0.0);
    std::vector<Composition> glacialDebris(n);
    std::vector<std::uint8_t> frozen(n, 0);
    int iceSubsteps = 0;
    // Coral reefs: the ring of step 2 and the shallow cells around it.
    std::vector<std::uint8_t> reef(n, 0);
    const bool reefGrowth = p.origin.type == "coral" && p.erosion.reef.enabled;
    if (reefGrowth) {
        const auto& landMask = world.layer(layers::kMask);
        constexpr int kReefMargin = 2;
        for (int j = 0; j < grid.ny; ++j)
            for (int i = 0; i < grid.nx; ++i) {
                if (landMask.at(i, j) < 0.5f)
                    continue;
                for (int nj = std::max(0, j - kReefMargin); nj <= std::min(grid.ny - 1, j + kReefMargin); ++nj)
                    for (int ni = std::max(0, i - kReefMargin); ni <= std::min(grid.nx - 1, i + kReefMargin); ++ni)
                        reef[nj * grid.nx + ni] = !fixed[nj * grid.nx + ni];
            }
    }
    // Time spent by each process (s), logged at the end.
    enum Process { kUplift, kClimate, kGlaciers, kRouting, kFluvial, kHillslope, kCoast, kMarine, kOther, kProcesses };
    double timing[kProcesses] = {};
    const char* timingNames[kProcesses] = {"uplift + rock", "climate",   "glaciers",       "routing",
                                           "fluvial",       "hillslopes", "wave erosion", "marine diffusion",
                                           "budget, checks, previews"};
    const double sedimentTan = std::tan(thermal.sedimentTalusAngleDeg * std::numbers::pi / 180.0);
    std::vector<double> hDiffused(n), hBefore(n);
    std::vector<Composition> removed(n);
    std::vector<std::uint8_t> onLand(n), slopeKind(n);
    std::vector<double> absorbed(n);
    double volumeError = 0.0; // largest relative volume error of the hillslope processes since the last log
    int diffusionSubsteps = 0;
    std::fill(source.begin(), source.end(), p.climate.basePrecipitationMPerYear * dx * dx); // until the climate runs
    FlowRouter router(grid.nx, grid.ny, dx);

    using Clock = std::chrono::steady_clock;
    auto logClock = Clock::now();
    int logSteps = 0;

    auto lap = Clock::now();
    const auto clock = [&](Process process) {
        const auto now = Clock::now();
        timing[process] += std::chrono::duration<double>(now - lap).count();
        lap = now;
    };
    for (int step = 0; step < steps; ++step) {
        const bool paused = context.tick();
        lap = Clock::now();
        double t0, length;
        stepSpan(step, t0, length);
        const double tMid = t0 + length / 2;
        hStart = h;

        // 1. Time parameters.
        const double sea = world.info().seaLevelM() + seaLevelOffsetM(p.timeline.seaLevelCurve, tMid);
        std::vector<double> activity(phases.size());
        for (std::size_t k = 0; k < phases.size(); ++k) {
            activity[k] = phaseActivity(phases[k], tMid);
            phaseDone[k] += activity[k] * length;
        }

        // 2. Uplift: U = sum_k rate_k * P_k * activity_k.
        if (processes.uplift) {
            for (int c = 0; c < n; ++c) {
                if (fixed[c])
                    continue;
                double uplift = 0.0, strata = 0.0;
                for (std::size_t k = 0; k < phases.size(); ++k) {
                    if (activity[k] == 0.0)
                        continue;
                    const double u = phases[k].rateMPerYear * patterns[k]->data()[c] * activity[k] * length;
                    uplift += u;
                    if (!faulted[k])
                        strata += u;
                }
                b[c] += uplift;
                cumUplift[c] += uplift;
                strataUplift[c] += strata;
            }
        }
        for (int c = 0; c < n; ++c)
            h[c] = b[c] + sed[c];
        updateMaterials();

        clock(kUplift);
        // 3. Climate, every climate_update_interval_steps: temperature on the
        //    surface s = h + ice, orographic precipitation; the discharge is
        //    weighted by the rain (erosion.fluvial.precipitation_weighted).
        currentSea = sea;
        const bool climateUpdate = step % std::max(1, sim.climateUpdateIntervalSteps) == 0;
        const double glacialOffset = glacialOffsetC(p.timeline.glaciations, tMid);
        if (climateUpdate && (processes.climate || processes.glaciers)) {
            // Altitudes from the world's sea level (where base_temperature_c holds).
            std::vector<std::uint8_t> water(n);
            parallelFor(n, [&](int c0, int c1) {
                for (int c = c0; c < c1; ++c) {
                    surface[c] = h[c] + ice[c] - world.info().seaLevelM();
                    water[c] = h[c] <= sea;
                }
            });
            climateModel.temperature(surface, glacialOffset, temperature);
            if (processes.climate)
                climateModel.precipitation(surface, temperature, water, p.climate.baseTemperatureC + glacialOffset,
                                           precipitation);
        }
        clock(kClimate);

        // 4. Glaciers, at the climate updates: the ice relaxes to equilibrium
        //    (or melts entirely when no cell is below 0 °C).
        if (processes.glaciers && climateUpdate) {
            const bool cold = std::any_of(temperature.begin(), temperature.end(), [](double t) { return t < 0.0; });
            if (cold) {
                iceSubsteps = glacier.relax(h, precipitation,
                                            {p.climate.baseTemperatureC, glacialOffset, p.climate.lapseRateCPerKm,
                                             world.info().seaLevelM(), sea});
            } else if (glacier.hasIce()) {
                glacier.meltAll(sim.climateUpdateIntervalSteps * length);
            }
            glacier.fineFields(ice, sliding, iceMelt, massBalance);
            for (int c = 0; c < n; ++c)
                frozen[c] = ice[c] > glacial.iceThresholdM;
        }
        if (climateUpdate && (processes.climate || processes.glaciers)) {
            // Discharge sources: the rain (or a uniform rain), plus the melt water.
            for (int c = 0; c < n; ++c)
                source[c] = ((p.erosion.fluvial.precipitationWeighted && processes.climate
                                  ? precipitation[c]
                                  : p.climate.basePrecipitationMPerYear) +
                             (processes.glaciers ? iceMelt[c] : 0.0)) *
                            dx * dx;
        }
        clock(kGlaciers);

        // 5. Routing, then fluvial erosion and deposition (Yuan et al. 2019,
        //    see Fluvial.hpp).
        const std::vector<double> bStart = b, sedStart = sed; // for the budget
        double exported = 0.0, reefAdded = 0.0;

        // Glacial erosion (Harbor 1992), sediment first, with the sliding of the
        //    last relaxation; the debris is carried by the ice to its front.
        std::fill(glacialErosion.begin(), glacialErosion.end(), 0.0);
        if (processes.glaciers && glacier.hasIce()) {
            const double coeff = glacial.erosionCoeff * multiply.glacial;
            for (int c = 0; c < n; ++c) {
                glacialEroded[c] = 0.0;
                if (fixed[c] || ice[c] <= 0.0 || sliding[c] <= 0.0)
                    continue;
                const double rate = coeff * std::pow(sliding[c], glacial.erosionExponent);
                glacialEroded[c] = rate * length;
                glacialDebris[c] = regolith.remove(c, glacialEroded[c]);
                glacialErosion[c] = rate;
                h[c] = b[c] + sed[c];
            }
            exported += glacier.depositDebris(regolith, glacialEroded, glacialDebris, ice, glacial.iceThresholdM, sea, h);
        }
        clock(kGlaciers);
        std::fill(erosionRate.begin(), erosionRate.end(), 0.0);
        std::fill(depositionRate.begin(), depositionRate.end(), 0.0);
        std::fill(sedimentFlux.begin(), sedimentFlux.end(), 0.0);
        if (processes.fluvial) {
            router.route(h, fixed, sea);
            router.accumulate(source, discharge);
            router.accumulate(cellAreas, drainageArea);
            clock(kRouting);
            for (int c = 0; c < n; ++c) {
                lakeDepth[c] = router.water()[c] - h[c];
                rockK[c] = kBase * rocks[material[c]].erodibility;
            }
            fluvial.dt = length;
            fluvial.seaLevel = sea;
            const FluvialResult result = fluvialStep(router, fluvial, regolith, discharge, drainageArea, rockK, fixed,
                                                     h, sedimentFlux, erosionRate, depositionRate,
                                                     processes.glaciers ? &frozen : nullptr);
            exported += result.exported;
            depositionIterations += result.iterations;
        }

        // The sea of this step: the ocean found by the routing (cells below sea
        // level connected to the border), fixed for the whole step. A river
        // mouth that the fluvial deposit raised above sea level stays a sea
        // cell until the marine diffusion spreads its load; seen as land, the
        // hillslopes would pour it onto the coast.
        for (int c = 0; c < n; ++c)
            inSea[c] = !fixed[c] && (processes.fluvial ? router.ocean()[c] != 0 : h[c] <= sea);

        clock(kFluvial);
        // 6. Hillslopes, on land: diffusion, then thermal collapse (not under the ice).
        if (processes.hillslope) {
            double moved = 0.0;
            hBefore = h;
            // Land diffuses; the sea (and below-sea lakes) absorbs what crosses
            // the coast, as sediment, then spread by the marine diffusion.
            for (int c = 0; c < n; ++c)
                slopeKind[c] = fixed[c] || frozen[c] ? 0 : inSea[c] || h[c] <= sea ? 2 : 1;
            // For the land, the whole sea is one fixed surface at sea level. With
            // each sea cell at its own depth, a coastal cell between a shallow
            // and a deep one would pipe sediment from the first (dug into its
            // rock) to the second, every sub-step: an unbounded transfer that
            // grew without limit. At sea level, the land (above it) only
            // ever loses to the sea, and a river mouth raised by its load does
            // not pour it back onto the coast.
            hDiffused = h;
            for (int c = 0; c < n; ++c)
                if (slopeKind[c] == 2)
                    hDiffused[c] = sea;
            std::fill(absorbed.begin(), absorbed.end(), 0.0);
            diffusionSubsteps =
                diffuseADI(hDiffused, slopeKind, grid.nx, grid.ny, diffusivity * length / (dx * dx), &absorbed);
#ifndef NDEBUG
            for (int c = 0; c < n; ++c)
                if (slopeKind[c] == 2 && absorbed[c] < -1e-9)
                    throw WorldError("the sea gave volume back to the land at step " + std::to_string(step + 1));
#endif
            for (int c = 0; c < n; ++c) {
                onLand[c] = slopeKind[c] == 1;
                if (slopeKind[c] == 2)
                    hDiffused[c] = h[c] + absorbed[c];
            }
            // Bookkeeping: lowered cells lose sediment, then rock; raised cells
            // gain sediment. Its composition is that of what was removed
            // nearby (5x5 cells), else of the cell's own cover or rock.
            parallelFor(n, [&](int c0, int c1) {
                for (int c = c0; c < c1; ++c) {
                    removed[c] = {};
                    // (the sea only absorbs: absorbed >= 0 up to rounding)
                    if (slopeKind[c] != 0 && hDiffused[c] < h[c])
                        removed[c] = regolith.remove(c, h[c] - hDiffused[c]);
                }
            });
            parallelFor(grid.ny, [&](int j0, int j1) {
                for (int j = j0; j < j1; ++j) {
                    for (int i = 0; i < grid.nx; ++i) {
                        const int c = j * grid.nx + i;
                        const double gain = hDiffused[c] - h[c];
                        if (slopeKind[c] == 0 || gain <= 0.0)
                            continue;
                        Composition nearby{};
                        double total = 0.0;
                        for (int nj = std::max(0, j - 2); nj <= std::min(grid.ny - 1, j + 2); ++nj)
                            for (int ni = std::max(0, i - 2); ni <= std::min(grid.nx - 1, i + 2); ++ni)
                                for (int k = 0; k < kSedimentKinds; ++k) {
                                    nearby[k] += removed[nj * grid.nx + ni][k];
                                    total += removed[nj * grid.nx + ni][k];
                                }
                        Composition volumes{};
                        for (int k = 0; k < kSedimentKinds; ++k)
                            volumes[k] = total > 0.0 ? gain * nearby[k] / total
                                         : sed[c] > 0.0 ? gain * frac[c][k]
                                                        : (k == rockKind[c] ? gain : 0.0);
                        regolith.deposit(c, volumes);
                    }
                }
            });
            for (int c = 0; c < n; ++c) {
                if (slopeKind[c] != 0) {
                    moved += std::abs(hDiffused[c] - h[c]);
                    h[c] = b[c] + sed[c];
                }
            }

            for (int pass = 0; pass < thermal.iterationsPerStep; ++pass) {
                for (int c = 0; c < n; ++c)
                    onLand[c] = !fixed[c] && !inSea[c] && !frozen[c] && h[c] > sea;
                moved += thermalCollapse(regolith, h, onLand, fixed, rockTan, sedimentTan, thermalRate, grid.nx,
                                         grid.ny, dx);
            }

            // Both processes only move material: the volume must not change.
            // (Sum of the changes, not difference of two sums of heights: those
            // are ~1e7 m on a grid, and their rounding would hide the error.)
            double change = 0.0;
            for (int c = 0; c < n; ++c)
                if (!fixed[c])
                    change += h[c] - hBefore[c];
            if (moved > 0.0)
                volumeError = std::max(volumeError, std::abs(change) / moved);
#ifndef NDEBUG
            for (int c = 0; c < n; ++c)
                if (sed[c] < 0.0)
                    throw WorldError("negative sediment at step " + std::to_string(step + 1));
#endif
        }

        clock(kHillslope);
        // 7. Marine processes: wave erosion of the coasts (cliff retreat), then
        //    diffusion of the sediment, the debris included.
        if (processes.marine) {
            coastSettings.seaLevel = sea;
            coastSettings.dt = length;
            if (step % p.erosion.coastal.exposureUpdateIntervalSteps == 0)
                coastal.updateExposure(h, inSea, coastSettings);
            coastDestroyed += coastal.step(regolith, h, inSea, fixed, rockHardness, coastSettings);
            exported += coastal.exported();
        }
        clock(kCoast);
        if (processes.marine) {
            marine.dt = length;
            marine.seaLevel = sea;
            exported += marineDiffusion(regolith, h, inSea, fixed, grid.nx, grid.ny, marine);
        }
        // Coral reefs grow up to the sea level, between max_depth and the surface
        //    (Darwin: the reef keeps up with the subsidence and the sea level).
        if (reefGrowth) {
            for (int c = 0; c < n; ++c) {
                const double depth = sea - h[c];
                if (!reef[c] || depth <= 0.0 || depth >= p.erosion.reef.maxDepthM)
                    continue;
                const double growth = std::min(p.erosion.reef.growthRate * length, depth);
                b[c] += growth;
                h[c] += growth;
                reefAdded += growth * cellArea;
            }
        }
        clock(kMarine);

        // Budget: the rock removed (beyond the uplift) is either still stored as
        // sediment or has left the domain.
        {
            double rockLost = 0.0, stored = 0.0;
            for (int c = 0; c < n; ++c) {
                if (fixed[c])
                    continue;
                rockLost += bStart[c] - b[c];
                stored += sed[c] - sedStart[c];
            }
            rockLost = rockLost * cellArea + reefAdded; // the reef's growth is new rock, not eroded
            stored *= cellArea;
            budgetRock += rockLost;
            budgetStored += stored;
            budgetExported += exported;
            budgetDrift += rockLost - stored - exported;
        }

        // 8. Checks, logs, preview.
        time = t0 + length;
        stepsDone = step + 1;
        for (int c = 0; c < n; ++c) {
            if (!std::isfinite(h[c])) {
                syncLayers();
                const auto dump = std::filesystem::path(p.debug.cacheDir) /
                                  ("simulation_nan_step_" + std::to_string(step + 1) + ".wgw");
                try {
                    writeWorld(world, dump);
                } catch (const std::exception&) {
                }
                throw WorldError("non-finite height at step " + std::to_string(step + 1) + " (cell " +
                                 std::to_string(c % grid.nx) + ", " + std::to_string(c / grid.nx) +
                                 "), state dumped to " + dump.string());
            }
        }
        ++logSteps;
        if (stepsDone % sim.logIntervalSteps == 0 || stepsDone == steps) {
            for (int c = 0; c < n; ++c)
                if (fixed[c] && (b[c] != fixedHeights[c] || sed[c] != 0.0))
                    throw WorldError("a FIXED cell changed at step " + std::to_string(stepsDone));
            double maxH = -1e300, sumRate = 0.0;
            int land = 0;
            for (int c = 0; c < n; ++c) {
                maxH = std::max(maxH, h[c]);
                if (h[c] > sea && !fixed[c]) {
                    sumRate += std::abs(h[c] - hStart[c]) / length;
                    ++land;
                }
            }
            const double seconds = std::chrono::duration<double>(Clock::now() - logClock).count();
            char line[384];
            double sedVolume = 0.0;
            for (int c = 0; c < n; ++c)
                sedVolume += sed[c];
            totalRock += budgetRock;
            totalDrift += budgetDrift;
            std::snprintf(line, sizeof line,
                          "t = %-9s step %d/%d  max h %.1f m  mean |dh/dt| %.3g m/yr  sed %.3g m3  "
                          "hillslope dV/V %.1e (ADI x%d)  %.1f ms/step",
                          formatYears(time).c_str(), stepsDone, steps, maxH, land ? sumRate / land : 0.0,
                          sedVolume * dx * dx, volumeError, diffusionSubsteps, 1000.0 * seconds / logSteps);
            context.log(line);
            std::snprintf(line, sizeof line,
                          "  budget: rock eroded %.3g m3 = stored %.3g + exported %.3g, drift %.1e (total %.1e)  "
                          "deposition iterations %.1f/step  sea level %+.1f m  cliffs %.3g m3",
                          budgetRock, budgetStored, budgetExported, budgetRock > 0.0 ? budgetDrift / budgetRock : 0.0,
                          totalRock > 0.0 ? totalDrift / totalRock : 0.0,
                          static_cast<double>(depositionIterations) / logSteps, sea - world.info().seaLevelM(),
                          coastDestroyed);
            coastDestroyed = 0.0;
            volumeError = 0.0;
            budgetRock = budgetStored = budgetExported = budgetDrift = 0.0;
            depositionIterations = 0;
            context.log(line);
            if (processes.glaciers) {
                const auto& ib = glacier.budget();
                const double volume = glacier.volume();
                const double expected = ib.accumulated - ib.ablated - ib.calved - ib.melted + ib.clamped;
                std::snprintf(line, sizeof line,
                              "  ice: %.3g m3 (max %.0f m)  accumulated %.3g  ablated %.3g  calved %.3g  melted %.3g  "
                              "drift %.1e  T offset %+.1f °C  relaxation %d sub-steps",
                              volume, *std::max_element(ice.begin(), ice.end()), ib.accumulated, ib.ablated, ib.calved,
                              ib.melted, ib.accumulated > 0.0 ? (volume - expected) / ib.accumulated : 0.0,
                              glacialOffset, iceSubsteps);
                context.log(line);
            }
            logClock = Clock::now();
            logSteps = 0;
        }
        context.progress(static_cast<float>(stepsDone) / steps, paused, syncLayers);
        clock(kOther);
    }
    {
        double total = 0.0;
        for (double t : timing)
            total += t;
        std::string report = "time per process:";
        for (int k = 0; k < kProcesses; ++k) {
            char item[96];
            std::snprintf(item, sizeof item, "  %s %.1f s (%.0f %%)", timingNames[k], timing[k],
                          total > 0.0 ? 100.0 * timing[k] / total : 0.0);
            report += item;
        }
        context.log(report);
    }

    // --- Final state ---------------------------------------------------------------
    updateMaterials();
    syncLayers();
    context.progress(1.0f, true);
}

} // namespace wg
