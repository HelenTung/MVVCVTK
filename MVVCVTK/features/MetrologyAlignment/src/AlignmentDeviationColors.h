#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
namespace AlignmentDeviationColors {
using Color = std::array<std::uint8_t, 3>;
inline constexpr Color invalidColor{128, 128, 128}, selectedColor{255, 255, 0};
inline constexpr const char *version = "deviation-colors-1";
bool GetDisplayValid(const AlignmentDeviationDisplay &display,
                     const AlignmentDeviationSnapshot &result);
Color ColorAt(double value, const AlignmentDeviationDisplay &display,
              const std::optional<std::array<double, 2>> &tolerance);
std::array<Color, 257>
GetLegend(const AlignmentDeviationDisplay &display,
          const std::optional<std::array<double, 2>> &tolerance);
} // namespace AlignmentDeviationColors
