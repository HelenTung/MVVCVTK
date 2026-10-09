#pragma once
#include "FeatureSupport/WorkLimit.h"

#include "SurfaceRecipe.h"
#include "Host/RoiReadTypes.h"
#include "Data/DataPayloads.h"
#include <functional>

struct SurfaceSeedTriangle final
{
    std::array<std::uint32_t, 3> vertices{};
};

struct SurfaceSeedGrid final
{
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> origin{};
    std::array<double, 9> indexToModel{}, modelToIndex{};
    const void *values = nullptr;
    double (*readScalar)(const void *, std::size_t) = nullptr;
    const unsigned char *validity = nullptr;
    // 借用请求内不可变的源节点；缺省仍可使用回调 reader。
    const void *scalarValues = nullptr;
    int scalarType = 0;
    const std::array<double, 2> *materialRange = nullptr;
};

enum class SurfaceSeedStatus
{
    Succeeded,
    Cancelled,
    InvalidInput,
    NoSurface,
    BudgetExceeded
};

class SurfaceSeedBuilder final
{
  public:
    static SurfaceSeedStatus BuildMesh(const SurfaceSeedGrid &grid, double iso,
                                       const RoiReadSnapshot &roi, double haloModel,
                                       std::uint32_t blockDepth, WorkLimit budgetBytes,
                                       const std::function<bool()> &cancelled,
                                       std::vector<std::array<double, 3>> &points,
                                       std::vector<SurfaceSeedTriangle> &triangles,
                                       SurfaceExecutionStats &statistics);
};
