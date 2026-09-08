#include "SurfaceRecipe.h"
#include <type_traits>
static_assert(std::is_default_constructible_v<SurfaceRecipe>);
static_assert(std::is_default_constructible_v<SurfaceProfileDiagnostic>);
static_assert(static_cast<unsigned>(SurfaceDeterminationMethod::AutomaticIso50) == 3);
