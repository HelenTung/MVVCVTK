#include "AlignmentDeviationColors.h"
#include <algorithm>
#include <cmath>
namespace AlignmentDeviationColors {
Color Mix(Color a, Color b, double fraction) {
  Color result{};
  fraction = std::clamp(fraction, 0.0, 1.0);
  for (std::size_t i = 0; i < 3; ++i)
    result[i] = static_cast<unsigned char>(
        std::lround(a[i] + (b[i] - a[i]) * fraction));
  return result;
}
Color ColorAt(double value, const AlignmentDeviationDisplay &display,
              const std::optional<std::array<double, 2>> &tolerance) {
  const double low = display.range[0], high = display.range[1];
  if (display.useToleranceColors && tolerance) {
    if (value >= (*tolerance)[0] && value <= (*tolerance)[1])
      return {60, 180, 95};
    if (value < (*tolerance)[0])
      return Mix({45, 90, 215}, {150, 205, 255},
                 (*tolerance)[0] > low ? (value - low) / ((*tolerance)[0] - low)
                                       : 0);
    return Mix({255, 200, 130}, {210, 45, 35},
               high > (*tolerance)[1]
                   ? (value - (*tolerance)[1]) / (high - (*tolerance)[1])
                   : 1);
  }
  const double center = std::clamp(0.0, low, high);
  if (value < center)
    return Mix({45, 90, 215}, {235, 235, 235},
               center > low ? (value - low) / (center - low) : 0);
  return Mix({235, 235, 235}, {210, 45, 35},
             high > center ? (value - center) / (high - center) : 1);
}
bool GetDisplayValid(const AlignmentDeviationDisplay &d,
                     const AlignmentDeviationSnapshot &r) {
  if (!r.forward || !r.parameters || !GetDataRevisionRefValid(d.result) ||
      d.result != r.result || !std::isfinite(d.range[0]) ||
      !std::isfinite(d.range[1]) || d.range[0] >= d.range[1] ||
      !std::isfinite(d.range[1] - d.range[0]) ||
      (r.parameters->metric == AlignmentDeviationMetric::UnsignedDistance &&
       d.range[0] < 0) ||
      (d.selectedVertex && *d.selectedVertex >= r.forward->samples.size()))
    return false;
  return !d.useToleranceColors || !r.parameters->tolerance ||
         (d.range[0] <= (*r.parameters->tolerance)[0] &&
          d.range[1] >= (*r.parameters->tolerance)[1]);
}
std::array<Color, 257>
GetLegend(const AlignmentDeviationDisplay &display,
          const std::optional<std::array<double, 2>> &tolerance) {
  std::array<Color, 257> result{};
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = ColorAt(display.range[0] +
                            (display.range[1] - display.range[0]) * (i / 256.0),
                        display, tolerance);
  return result;
}
} // namespace AlignmentDeviationColors
