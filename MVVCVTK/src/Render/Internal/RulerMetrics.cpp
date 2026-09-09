#include "Render/Internal/RulerMetrics.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

bool RulerMetrics::GetParamsValid(const RulerParams& params) noexcept
{
    switch (params.unit) {
    case RulerUnit::Auto: case RulerUnit::Millimeter: case RulerUnit::Micrometer: break;
    default: return false;
    }
    switch (params.position) {
    case RulerPosition::BottomLeft: case RulerPosition::BottomRight:
    case RulerPosition::TopLeft: case RulerPosition::TopRight: break;
    default: return false;
    }
    return std::isfinite(params.targetPixels)
        && params.targetPixels >= 40.0 && params.targetPixels <= 2000.0
        && params.fontSize >= 8 && params.fontSize <= 64
        && std::all_of(params.color.begin(), params.color.end(), [](double value) {
            return std::isfinite(value) && value >= 0.0 && value <= 1.0;
        });
}

bool RulerMetrics::GetGeometryValid(const GridGeometry3D& geometry) noexcept
{
    if (!GetGridGeometryValid(geometry)) return false;
    std::array<double, 16> matrix{ 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            matrix[row * 4 + col] = geometry.direction[row * 3 + col];
        }
    }
    std::array<double, 9> inverse{};
    return GetInverse(matrix, inverse);
}

bool RulerMetrics::GetInverse(const std::array<double, 16>& matrix,
    std::array<double, 9>& inverse) noexcept
{
    if (!std::all_of(matrix.begin(), matrix.end(), [](double value) {
            return std::isfinite(value);
        }) || matrix[12] != 0.0 || matrix[13] != 0.0
        || matrix[14] != 0.0 || matrix[15] != 1.0) return false;

    // 先按列归一化，det 阈值不随纯展示缩放的数量级漂移。
    std::array<double, 3> scales{};
    std::array<double, 9> a{};
    for (int col = 0; col < 3; ++col) {
        scales[col] = std::hypot(matrix[col], matrix[4 + col], matrix[8 + col]);
        if (!std::isfinite(scales[col]) || scales[col] <= 0.0) return false;
        for (int row = 0; row < 3; ++row) a[row * 3 + col] = matrix[row * 4 + col] / scales[col];
    }
    const double det = a[0] * (a[4]*a[8] - a[5]*a[7])
        - a[1] * (a[3]*a[8] - a[5]*a[6]) + a[2] * (a[3]*a[7] - a[4]*a[6]);
    if (!std::isfinite(det) || std::abs(det) <= 1e-12) return false;
    inverse = { a[4]*a[8]-a[5]*a[7], a[2]*a[7]-a[1]*a[8], a[1]*a[5]-a[2]*a[4],
        a[5]*a[6]-a[3]*a[8], a[0]*a[8]-a[2]*a[6], a[2]*a[3]-a[0]*a[5],
        a[3]*a[7]-a[4]*a[6], a[1]*a[6]-a[0]*a[7], a[0]*a[4]-a[1]*a[3] };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) inverse[row * 3 + col] /= det * scales[row];
    }
    return std::all_of(inverse.begin(), inverse.end(), [](double value) { return std::isfinite(value); });
}

double RulerMetrics::GetLength(const std::array<double, 9>& inverse,
    const std::array<double, 3>& worldDelta) noexcept
{
    std::array<double, 3> delta{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) delta[row] += inverse[row * 3 + col] * worldDelta[col];
    }
    return std::hypot(delta[0], delta[1], delta[2]);
}

RulerState RulerMetrics::BuildState(const double mmPerPixel,
    const double availablePixels, const RulerParams& params, const double previousLengthMm)
{
    RulerState state;
    state.status = RulerStatus::InvalidScale;
    if (!GetParamsValid(params) || !std::isfinite(mmPerPixel) || mmPerPixel <= 0.0
        || !std::isfinite(availablePixels)) return state;
    if (availablePixels < 40.0) {
        state.status = RulerStatus::ViewportTooSmall;
        return state;
    }
    const double target = std::min(params.targetPixels, availablePixels);
    const double maxPixels = std::min(target * 1.35, availablePixels);
    const double oldPixels = previousLengthMm / mmPerPixel;
    double length = previousLengthMm;
    if (!std::isfinite(oldPixels) || oldPixels < target * 0.45 || oldPixels > maxPixels) {
        const double desired = target * mmPerPixel;
        // 有界显示域：避免极端输入导致 pow/格式化溢出或伪零刻度。
        if (!std::isfinite(desired) || desired < 1e-12 || desired > 1e12) return state;
        const double decade = std::pow(10.0, std::floor(std::log10(desired)));
        const double fraction = desired / decade;
        length = (fraction >= 5.0 ? 5.0 : fraction >= 2.0 ? 2.0 : 1.0) * decade;
    }
    const double pixels = length / mmPerPixel;
    if (!std::isfinite(pixels) || pixels <= 0.0 || pixels > availablePixels) return state;
    const bool isMicrometer = params.unit == RulerUnit::Micrometer
        || (params.unit == RulerUnit::Auto && length < 1.0);
    const double value = length * (isMicrometer ? 1000.0 : 1.0);
    std::ostringstream text;
    text.imbue(std::locale::classic());
    if (value < 0.0001 || value >= 1e7) {
        text << std::scientific << std::setprecision(0) << value;
    } else {
        const int decimals = std::max(0, -static_cast<int>(std::floor(std::log10(value))));
        text << std::fixed << std::setprecision(decimals) << value;
    }
    text << (isMicrometer ? " um" : " mm");
    state.status = RulerStatus::Visible;
    state.lengthMm = length;
    state.lengthPixels = pixels;
    state.label = text.str();
    return state;
}
