#pragma once

#include "AlignmentGeometry.h"
#include "PagedArray.h"
#include <limits>

struct AlignmentMeshHit final {
    AlignmentPoint point{};
    AlignmentPoint normal{};
    std::uint64_t triangle = 0;
    double distanceSquared = std::numeric_limits<double>::infinity();
    bool isValid = false;
    std::array<AlignmentPoint, 3> observableDirections{};
    std::size_t observableCount = 0;
};
class AlignmentCudaQuery;

// 只读三角面索引；共享输入几何，查询期间不分配、不改变输入，也不依赖显示 LOD。
class AlignmentMeshQuery final {
  public:
    AlignmentMeshQuery(std::shared_ptr<const SurfaceMeshPayload> mesh,
                       const AlignmentWork &work,
                       const std::vector<std::uint64_t> &triangles = {});
    AlignmentMeshHit GetClosest(const AlignmentPoint &point, bool isObservabilityNeeded = false) const;
    AlignmentMeshHit GetHit(const AlignmentPoint &point, std::uint64_t triangle, bool isObservabilityNeeded = false) const;
    std::size_t GetWorkingBytes() const noexcept;
    bool GetIsEmpty() const noexcept { return m_triangles.empty(); }
    std::unique_ptr<AlignmentCudaQuery> BuildCudaQuery(std::size_t batchCapacity) const;

  private:
    struct Node final {
        std::array<double, 6> bounds{};
        std::size_t first = 0, count = 0;
        std::size_t left = 0, right = 0;
    };
    std::size_t BuildNode(std::size_t first, std::size_t count);
    std::array<AlignmentPoint, 3> GetTriangle(std::uint64_t triangle) const;
    void CheckWork() const;
    std::shared_ptr<const SurfaceMeshPayload> m_mesh;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    PagedArray<std::uint64_t> m_triangles;
    PagedArray<Node> m_nodes;
};
