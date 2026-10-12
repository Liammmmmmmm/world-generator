#include "wg/gen/stages/ErosionStage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "wg/core/WorldState.hpp"
#include "wg/erosion/ParticleErosion.hpp"
#include "wg/erosion/Rock.hpp"
#include "wg/island/Geology.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

namespace {

std::string format(const char* pattern, auto... args) {
    char buffer[256];
    std::snprintf(buffer, sizeof buffer, pattern, args...);
    return buffer;
}

} // namespace

void ErosionStage::run(WorldState& world, const StageContext& context) {
    const IslandParams& island = world.params().values;
    const auto& params = island.erosion.particles;
    const auto& thermal = island.erosion.thermal;
    Grid2D<float>& h = world.editLayer(layers::kHeight);
    const Grid2D<float>& hInitial = world.layer(layers::kHeightInitial);
    const Grid2D<std::uint8_t>& flags = world.byteLayer(layers::kFlags);
    const int nx = h.width(), ny = h.length();
    Grid2D<float>& sediment = world.createLayer(layers::kSediment, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& discharge = world.createLayer(layers::kDischargeNorm, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& eroded = world.createLayer(layers::kEroded, nx, ny, 0.0f, {LayerStyle::Diverging, {}});
    Grid2D<float>& momentum = world.createLayer(layers::kMomentum, nx, ny, 0.0f,
                                                {LayerStyle::Direction, {std::string(layers::kMomentumNorm)}});
    Grid2D<float>& momentumNorm = world.createLayer(layers::kMomentumNorm, nx, ny, 0.0f, {LayerStyle::Scalar, {}});
    Grid2D<float>& surfaceMaterial = world.editLayer(layers::kSurfaceMaterial);

    // To the normalized frame: cells 1 unit wide, heights in height_scale_m
    // from sea level.
    const double sea = context.info.seaLevelM();
    const double scale = params.heightScaleM;
    erosion::ErosionMap map(nx, ny);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            map.at(i, j).height = static_cast<float>((h.at(i, j) - sea) / scale);
            if (flags.at(i, j) & layers::kFixed)
                map.setFixed(i, j);
        }
    }

    // The rock under the sediment (volcanic strata follow the initial surface).
    const Geology geology(island, &hInitial);
    const erosion::Rock rock(geology, island.geology.materials,
                             {nx, ny, context.info.widthM() / nx, scale, sea}, params.hardnessInfluence,
                             thermal.sedimentTalusAngleDeg, thermal.underwaterTalusAngleDeg);

    // Back to meters, into the layers.
    const auto publish = [&] {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const erosion::ErosionCell& cell = map.at(i, j);
                h.at(i, j) = static_cast<float>(sea + cell.height * scale);
                sediment.at(i, j) = static_cast<float>(cell.sediment * scale);
                discharge.at(i, j) = std::erf(static_cast<float>(params.dischargeScale) * cell.discharge);
                eroded.at(i, j) = hInitial.at(i, j) - h.at(i, j);
                momentum.at(i, j) = std::atan2(cell.momentumY, cell.momentumX);
                momentumNorm.at(i, j) = std::hypot(cell.momentumX, cell.momentumY);
                surfaceMaterial.at(i, j) = static_cast<float>(rock.material(i, j, cell));
            }
        }
    };

    if (!params.enabled || params.steps == 0) {
        publish();
        return;
    }

    erosion::ParticleErosionParams settings;
    settings.drop = {params.maxAge,
                     static_cast<float>(params.gravity),
                     static_cast<float>(params.momentumTransfer),
                     static_cast<float>(params.entrainment),
                     static_cast<float>(params.depositionRate),
                     static_cast<float>(params.evaporationRate),
                     static_cast<float>(params.minVolume),
                     static_cast<float>(params.dischargeScale),
                     &rock,
                     thermal.enabled ? static_cast<float>(thermal.rate) : 0.0f,
                     params.underwaterSteps};
    settings.particlesPerStep = params.particlesPerStep;
    settings.dischargeLrate = static_cast<float>(params.dischargeLrate);
    erosion::ParticleErosion erosion(map, settings, context.seed);
    context.log(format("%d land cells, %d drops per step, %d steps", erosion.landCells(), erosion.particlesPerStep(),
                       params.steps));

    // Mass balance: all heights plus the sediment that left the domain.
    const double mass0 = erosion.totalMass();
    double magnitude = 0.0; // to express the drift relatively
    for (std::size_t k = 0; k < map.size(); ++k)
        magnitude += std::abs(map[k].height);
    const auto start = std::chrono::steady_clock::now();
    const int logEvery = std::max(1, params.steps / 10);
    for (int s = 0; s < params.steps; ++s) {
        erosion.step(s);
        if ((s + 1) % logEvery == 0 || s + 1 == params.steps) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double drift = erosion.totalMass() - mass0;
            context.log(format("step %d: %.2f ms/step, sediment out %.1f, mass drift %.3g (%.2g of sum |h|)", s + 1,
                               1000.0 * seconds / (s + 1), erosion.lostSediment(), drift,
                               drift / std::max(1.0, magnitude)));
        }
        context.step(s + 1, params.steps, publish);
    }
    publish();

    for (std::size_t k = 0; k < map.size(); ++k) {
        const erosion::ErosionCell& c = map[k];
        if (!std::isfinite(c.height) || !std::isfinite(c.discharge) || !std::isfinite(c.momentumX) ||
            !std::isfinite(c.momentumY))
            throw WorldError("non-finite value produced by the erosion");
    }
}

} // namespace wg
