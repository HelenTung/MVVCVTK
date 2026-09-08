// 测试用途：编译检查伪影校正公开头的自足性。
#include "Host/ArtifactReductionHostFeature.h"
#include <type_traits>
#if __has_include("remove_ring.h") || __has_include("ArtifactReductionAlgorithm.h") || __has_include("Data/DataManager.h")
#error "ArtifactReduction leaks a private include surface"
#endif
static_assert(std::is_base_of_v<HostFeature, ArtifactReductionHostFeature>);
static_assert(!std::is_copy_constructible_v<ArtifactReductionHostFeature>);
