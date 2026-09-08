#include "Host/WallThicknessHostFeature.h"
#include <type_traits>
static_assert(std::is_base_of_v<HostFeature, WallThicknessHostFeature>);
static_assert(!std::is_copy_constructible_v<WallThicknessHostFeature>);
