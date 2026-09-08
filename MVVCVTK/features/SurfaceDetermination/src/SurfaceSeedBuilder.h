#pragma once

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
    const LabelMap3DPayload *labels = nullptr;
    const SurfaceMeshPayload *initialMesh = nullptr;
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
                                       const std::optional<SurfaceMaterialPair> &materials,
                                       const RoiReadSnapshot &roi, double haloModel,
                                       std::uint32_t blockDepth, std::size_t budgetBytes,
                                       const std::function<bool()> &cancelled,
                                       std::vector<std::array<double, 3>> &points,
                                       std::vector<SurfaceSeedTriangle> &triangles,
                                       SurfaceExecutionStats &statistics);
    static std::uint64_t GetLabel(const LabelMap3DPayload &labels, std::size_t index);
};
