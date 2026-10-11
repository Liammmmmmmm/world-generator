#include "wg/sim/Climate.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

#include "wg/core/Parallel.hpp"

namespace wg {

double glacialOffsetC(const std::vector<IslandParams::Glaciation>& glaciations, double t) {
    double offset = 0.0;
    for (const auto& g : glaciations) {
        const double span = g.endYears - g.startYears;
        if (span <= 0.0 || t <= g.startYears || t >= g.endYears)
            continue;
        const double ramp = 0.1 * span;
        offset += g.temperatureOffsetC * std::min({1.0, (t - g.startYears) / ramp, (g.endYears - t) / ramp});
    }
    return offset;
}

namespace {

constexpr double kSaturationPerC = 0.07; // Clausius-Clapeyron, ~7 % per °C
constexpr double kUpwindCells = 1.5;     // the moisture of a cell comes from this far upwind
constexpr double kBlurM = 20.0;          // standard deviation of the blur of P
constexpr double kMinHumidity = 0.05;    // even a dry climate brings some moisture

// Separable Gaussian blur, sigma in cells.
void blur(std::vector<double>& field, int nx, int ny, double sigma) {
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    if (radius < 1)
        return;
    std::vector<double> kernel(2 * radius + 1);
    for (int k = -radius; k <= radius; ++k)
        kernel[k + radius] = std::exp(-0.5 * k * k / (sigma * sigma));
    std::vector<double> tmp(field.size());
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < nx; ++i) {
                double sum = 0.0, weight = 0.0;
                for (int k = -radius; k <= radius; ++k) {
                    const int x = i + k;
                    if (x < 0 || x >= nx)
                        continue;
                    sum += kernel[k + radius] * field[j * nx + x];
                    weight += kernel[k + radius];
                }
                tmp[j * nx + i] = sum / weight;
            }
    });
    parallelFor(ny, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < nx; ++i) {
                double sum = 0.0, weight = 0.0;
                for (int k = -radius; k <= radius; ++k) {
                    const int y = j + k;
                    if (y < 0 || y >= ny)
                        continue;
                    sum += kernel[k + radius] * tmp[y * nx + i];
                    weight += kernel[k + radius];
                }
                field[j * nx + i] = sum / weight;
            }
    });
}

} // namespace

ClimateModel::ClimateModel(const IslandParams& params, int nx, int ny, double cellM)
    : climate_(params.climate), nx_(nx), ny_(ny), cellM_(cellM) {
    // wind.direction_deg is where the wind comes from: the air moves the other way.
    const double variability = climate_.wind.variabilityDeg;
    const std::vector<double> offsets = variability > 0.0 ? std::vector<double>{-variability, 0.0, variability}
                                                          : std::vector<double>{0.0};
    for (const double offset : offsets) {
        const double a = (climate_.wind.directionDeg + 180.0 + offset) * std::numbers::pi / 180.0;
        Pass pass{std::cos(a), std::sin(a), std::vector<int>(static_cast<std::size_t>(nx) * ny)};
        std::iota(pass.order.begin(), pass.order.end(), 0);
        std::sort(pass.order.begin(), pass.order.end(), [&](int a, int b) {
            return (a % nx) * pass.wx + (a / nx) * pass.wy < (b % nx) * pass.wx + (b / nx) * pass.wy;
        });
        passes_.push_back(std::move(pass));
    }
}

void ClimateModel::temperature(const std::vector<double>& surface, double offsetC, std::vector<double>& out) const {
    out.resize(surface.size());
    for (std::size_t c = 0; c < surface.size(); ++c)
        out[c] = climate_.baseTemperatureC + offsetC - climate_.lapseRateCPerKm * std::max(surface[c], 0.0) / 1000.0;
}

void ClimateModel::precipitation(const std::vector<double>& surface, const std::vector<double>& temperature,
                                 const std::vector<std::uint8_t>& sea, double seaTemperatureC,
                                 std::vector<double>& out) const {
    const int n = nx_ * ny_;
    const auto& oro = climate_.orographic;
    const double dxKm = cellM_ / 1000.0;
    const double humidity = std::max(climate_.humidity, kMinHumidity);
    const auto saturation = [&](double t) { return oro.q0 * std::exp(kSaturationPerC * t); };
    const double inflow = humidity * saturation(seaTemperatureC);
    const double recharge = std::min(1.0, oro.moistureRechargeOverSea * dxKm);

    out.assign(n, 0.0);
    // The passes (wind directions) are independent: one thread each. Within a
    // pass, each cell needs the cells upwind: sequential.
    std::vector<std::vector<double>> rains(passes_.size(), std::vector<double>(n));
    parallelFor(static_cast<int>(passes_.size()), [&](int p0, int p1) {
        for (int pi = p0; pi < p1; ++pi) {
            const Pass& pass = passes_[pi];
            std::vector<double>& rain = rains[pi];
            std::vector<double> q(n);
            std::fill(q.begin(), q.end(), inflow);
            for (const int c : pass.order) {
                const int i = c % nx_, j = c / nx_;
                // Incoming moisture, from 1.5 cells upwind (the edge: the open sea).
                const double fi = i - kUpwindCells * pass.wx, fj = j - kUpwindCells * pass.wy;
                double qIn = inflow;
                if (fi >= 0.0 && fj >= 0.0 && fi <= nx_ - 1 && fj <= ny_ - 1) {
                    const int i0 = std::min(static_cast<int>(fi), nx_ - 2), j0 = std::min(static_cast<int>(fj), ny_ - 2);
                    const double tx = fi - i0, ty = fj - j0;
                    const auto at = [&](int x, int y) { return q[y * nx_ + x]; };
                    qIn = (1 - ty) * ((1 - tx) * at(i0, j0) + tx * at(i0 + 1, j0)) +
                          ty * ((1 - tx) * at(i0, j0 + 1) + tx * at(i0 + 1, j0 + 1));
                }
                // Rise of the air over the cell (m), from the slope along the wind.
                const int il = std::max(i - 1, 0), ir = std::min(i + 1, nx_ - 1);
                const int jl = std::max(j - 1, 0), jr = std::min(j + 1, ny_ - 1);
                const double gx = (std::max(surface[j * nx_ + ir], 0.0) - std::max(surface[j * nx_ + il], 0.0)) /
                                  ((ir - il) * cellM_);
                const double gy = (std::max(surface[jr * nx_ + i], 0.0) - std::max(surface[jl * nx_ + i], 0.0)) /
                                  ((jr - jl) * cellM_);
                const double riseKm = std::max(0.0, pass.wx * gx + pass.wy * gy) * dxKm;
                double p = qIn * (oro.baseRate * dxKm + oro.condensationRate * riseKm) +
                           std::max(0.0, qIn - saturation(temperature[c]));
                p = std::min(p, qIn);
                double qOut = qIn - p;
                if (sea[c])
                    qOut += (humidity * saturation(temperature[c]) - qOut) * recharge;
                q[c] = qOut;
                rain[c] = p;
            }
        }
    }, 1);
    for (const auto& rain : rains)
        for (int c = 0; c < n; ++c)
            out[c] += rain[c];

    blur(out, nx_, ny_, std::max(1.0, kBlurM / cellM_)); // before the scaling: it moves rain to the sea
    // To m/yr: the mean over the land is base_precipitation_m_per_year (the
    // relief redistributes the rain, it does not change the island's budget,
    // which the erodibility k_base is calibrated against). Without land: a flat
    // coastal lowland (q = inflow, no rise) gets the base rate.
    double landRain = 0.0;
    int land = 0;
    for (int c = 0; c < n; ++c) {
        if (!sea[c]) {
            landRain += out[c];
            ++land;
        }
    }
    const double reference = land > 0 && landRain > 0.0 ? landRain / land : inflow * oro.baseRate * dxKm * passes_.size();
    for (double& p : out)
        p *= climate_.basePrecipitationMPerYear / reference;
}

} // namespace wg
