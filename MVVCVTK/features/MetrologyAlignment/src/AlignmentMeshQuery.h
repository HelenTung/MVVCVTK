#pragma once

#include "AlignmentGeometry.h"
#include <limits>

struct AlignmentMeshHit final {
    AlignmentPoint point{};
    AlignmentPoint normal{};
    std::uint64_t triangle = 0;
    double distanceSquared = std::numeric_limits<double>::infinity();
    bool isValid = false;
};
class AlignmentCudaQuery;

// 只读三角面索引；共享输入几何，查询期间不分配、不改变输入，也不依赖显示 LOD。
class AlignmentMeshQuery final {
  public:
    AlignmentMeshQuery(std::shared_ptr<const SurfaceMeshPayload> mesh,
                       const AlignmentWork &work,
                       const std::vector<std::uint64_t> &triangles = {});
    AlignmentMeshHit GetClosest(const AlignmentPoint &point) const;
    AlignmentMeshHit GetHit(const AlignmentPoint &point, std::uint64_t triangle) const;
    std::size_t GetWorkingBytes() const noexcept;
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
    std::vector<std::uint64_t> m_triangles;
    std::vector<Node> m_nodes;
};
