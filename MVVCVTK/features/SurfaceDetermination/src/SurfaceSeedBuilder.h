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

// 仓内审计信息，不进入公开 generation/API/SPI。
struct SurfaceSeedAudit final {
    std::size_t classificationCacheBytes = 0;
    std::uint64_t classifiedNodeCount = 0;
    std::uint64_t uniformCellCount = 0;
    std::size_t budgetFallbackCount = 0;
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
    // 请求只能缩小私有配置上限；0 保留原标量扫描，用于回退和数值对照。
    std::size_t classificationCacheLimit = SIZE_MAX;
    SurfaceSeedAudit *audit = nullptr;
    // 仓内故障测试可在可选缓存分配前抛 bad_alloc；缺省不产生额外行为。
    void (*cacheAllocationCheckpoint)(std::size_t) = nullptr;
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
