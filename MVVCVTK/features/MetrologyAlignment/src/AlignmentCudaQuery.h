#pragma once
#include "Data/DataPayloads.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

// 仅仓内私有 POD 边界；CUDA 类型、句柄与依赖不进入 Feature API/SPI。
struct AlignmentCudaNode final {
    double bounds[6];
    std::uint64_t first, count, left, right;
};
struct AlignmentCudaHit final {
    double distanceSquared;
    std::uint64_t triangle;
    int error;
    bool requiresVerification;
};
class AlignmentCudaUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class AlignmentCudaQuery final {
public:
    AlignmentCudaQuery(MeshReadArray<AlignmentCudaNode> nodes,MeshReadArray<std::uint64_t> order,
                       MeshReadArray<double> vertices,MeshReadArray<std::uint64_t> triangles,
                       std::size_t batchCapacity,const std::function<void()>& checkpoint={});
    ~AlignmentCudaQuery();
    AlignmentCudaQuery(const AlignmentCudaQuery &) = delete;
    AlignmentCudaQuery &operator=(const AlignmentCudaQuery &) = delete;
    void Query(const double *points, std::size_t count, AlignmentCudaHit *hits);
    static bool GetAvailable(std::string &reason);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
