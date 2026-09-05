#include "Host/SurfaceDeterminationHostFeature.h"

#include <memory>
#include <type_traits>
#include <utility>

static_assert(std::is_base_of_v<
    HostFeature, SurfaceDeterminationHostFeature>);
static_assert(std::is_same_v<
    decltype(std::declval<const SurfaceDeterminationHostFeature&>()
        .GetSurfaceSnapshot()),
    std::shared_ptr<const SurfaceGenerationSnapshot>>);
