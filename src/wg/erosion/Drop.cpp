#include "wg/erosion/Drop.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "wg/erosion/Avalanche.hpp"

namespace wg::erosion {

namespace {

// Drops always move by this distance per iteration (dynamic time step): about
// one cell, never skipping one.
constexpr float kStepLength = std::numbers::sqrt2_v<float>;

// Below this, the time-averaged flow of a cell is negligible (it decays
// towards denormal floats where nothing flows any more).
constexpr float kMinFlow = 1e-6f;

// Adds `amount` (> 0: deposit, < 0: erosion) to a cell. Erosion removes the
// loose sediment first. The cell then avalanches.
void addToCell(ErosionMap& map, const DropParams& p, int i, int j, float amount) {
    ErosionCell& cell = map.at(i, j);
    cell.height += amount;
    cell.sediment = std::max(0.0f, cell.sediment + amount);
    if (p.thermalRate > 0.0f && amount != 0.0f)
        avalanche(map, *p.rock, p.thermalRate, i, j);
}

} // namespace

float descend(ErosionMap& map, const DropParams& p, float x, float y) {
    float volume = 1.0f, sediment = 0.0f;
    Vec2 speed;
    for (int age = 0;; ++age) {
        const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y));
        ErosionCell& cell = map.at(i, j);

        // Death on the cell it is on: in the sea (for now), too old, or dried
        // out. What it carries stays here.
        if (cell.height < 0.0f || age > p.maxAge || volume < p.minVolume) {
            addToCell(map, p, i, j, sediment);
            return 0.0f;
        }

        // Gravity: down the surface normal n = normalize(-g.x, 1, -g.y).
        const Vec2 g = map.gradient(i, j);
        const float norm = std::sqrt(1.0f + g.x * g.x + g.y * g.y);
        speed.x += p.gravity * (-g.x / norm) / volume;
        speed.y += p.gravity * (-g.y / norm) / volume;

        // Momentum transfer: the stream drags the drop along, the more as they
        // already go the same way. The speed is higher on the outside of a
        // bend, and so is the erosion: bends grow by themselves (meanders).
        const float flow = std::hypot(cell.momentumX, cell.momentumY);
        const float own = std::hypot(speed.x, speed.y);
        if (flow > kMinFlow && own > 0.0f) {
            const float alignment = (cell.momentumX / flow) * (speed.x / own) + (cell.momentumY / flow) * (speed.y / own);
            const float pull = p.momentumTransfer * alignment / (volume + cell.discharge);
            speed.x += pull * cell.momentumX;
            speed.y += pull * cell.momentumY;
        }

        // Dynamic time step: move by about one cell.
        const float length = std::hypot(speed.x, speed.y);
        if (length == 0.0f) {
            addToCell(map, p, i, j, sediment);
            return 0.0f;
        }
        speed.x *= kStepLength / length;
        speed.y *= kStepLength / length;

        cell.dischargeTrack += volume;
        cell.momentumXTrack += volume * speed.x;
        cell.momentumYTrack += volume * speed.y;

        x += speed.x;
        y += speed.y;
        const int ni = static_cast<int>(std::floor(x)), nj = static_cast<int>(std::floor(y));
        if (!map.inside(ni, nj) || map.fixed(ni, nj))
            return sediment; // left the domain, with its sediment

        // Mass transfer on the cell it left: the drop tends to carry an
        // equilibrium amount, larger on steep slopes and in strong flows.
        const float hdiff = cell.height - map.at(ni, nj).height;
        const float dischargeNorm = std::erf(p.dischargeScale * cell.discharge);
        const float cEq = std::max(0.0f, (1.0f + p.entrainment * dischargeNorm) * hdiff);
        // > 0: erosion of the cell, < 0: deposit, at most what the drop carries.
        float transfer = std::max(p.depositionRate * (cEq * volume - sediment), -sediment);
        if (transfer > 0.0f) { // loose sediment at the normal rate, then the rock at its own
            const float loose = std::min(transfer, cell.sediment);
            transfer = loose + (transfer - loose) * p.rock->erodibility(i, j, cell);
        }
        sediment += transfer;
        addToCell(map, p, i, j, -transfer);

        // Evaporation (the sediment stays in the drop: mass conservative).
        volume *= 1.0f - p.evaporationRate;
    }
}

} // namespace wg::erosion
