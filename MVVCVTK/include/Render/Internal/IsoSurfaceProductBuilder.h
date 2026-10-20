#pragma once

#include "Render/Internal/RenderResourceCoordinator.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IsoSurfaceKey final {
    RenderInputStamp inputStamp;
    const void* inputIdentity = nullptr;
    const void* maskIdentity = nullptr;
    vtkMTimeType inputMTime = 0;
    vtkMTimeType inputScalarMTime = 0;
    vtkMTimeType maskMTime = 0;
    vtkMTimeType maskScalarMTime = 0;
    std::array<int, 3> outputDimensions{};
    double isoValue = 0.0;
};

// 可选CPU上传缓存独立于不可变几何；取走后的只读缓冲保持产品owner，
// 最后上传读者先释放内存，再同步归还预算。其余读者沿VTK原路径绘制。
class IsoSurfaceIndexCache final {
public:
    IsoSurfaceIndexCache(std::vector<unsigned int> indices,
        std::weak_ptr<const void> owner, std::function<void()> onReleased);
    ~IsoSurfaceIndexCache();
    std::shared_ptr<const std::vector<unsigned int>> TakeIndices();
    bool GetHasIndices() const;
    std::uint64_t GetExpectedBytes() const noexcept;
    // 仅owner提交后台重装配的独立缓冲；该缓冲由自己的任务租约计费。
    void SetPreparedIndices(std::shared_ptr<const std::vector<unsigned int>> indices);
    std::uint64_t GetBytes() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

struct IsoSurfaceProduct final {
    RenderInputUse inputUse;
    std::uint64_t requestRevision = 0;
    RenderInputStamp inputStamp;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    std::array<int, 3> inputDimensions{};
    double isoValue = 0.0;
    vtkSmartPointer<vtkPolyData> surface;
    // 单一 surface 的 VTK 三角形索引，vertex offset 为 0；时间为 0 表示走原路径。
    std::unique_ptr<IsoSurfaceIndexCache> triangleIndices;
    vtkMTimeType triangleIndicesTime = 0;
    std::uint64_t meshBytes = 0;
    std::uint64_t GetActualBytes() const noexcept
    { return meshBytes + (triangleIndices ? triangleIndices->GetBytes() : 0); }
    bool isPreview = false;
};

struct IsoSurfaceBuildRequest final {
    RenderInputUse inputUse;
    IsoSurfaceKey key;
    std::uint64_t requestRevision = 0;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    vtkSmartPointer<vtkImageData> input;
    vtkSmartPointer<vtkImageData> mask;
    bool isPreview = false;
    std::weak_ptr<RenderResourceCoordinator> resources;
};

struct IsoSurfaceBuildResult final {
    RenderProductFailure failureReason = RenderProductFailure::None;
    std::shared_ptr<const IsoSurfaceProduct> product;
    std::shared_ptr<const std::vector<unsigned int>> preparedIndices;
    std::string message;
};

class IsoSurfaceProductBuilder final {
public:
    // 首段只准入图像/计数工作区；worker 计数后、提取网格前追加网格预留。
    static std::optional<std::uint64_t> GetEstimatedBytes(
        const IsoSurfaceBuildRequest& request);
    static std::optional<std::uint64_t> GetIndexEstimatedBytes(
        const std::shared_ptr<const IsoSurfaceProduct>& product);
    // 复用不可变几何，只在worker私有壳上装配；失败只丢可选索引。
    std::shared_ptr<const std::vector<unsigned int>> BuildIndexCandidate(
        const std::shared_ptr<const IsoSurfaceProduct>& product,
        const RenderTaskToken& stopToken) const;
    IsoSurfaceBuildResult BuildProduct(
        const IsoSurfaceBuildRequest& request,
        const RenderTaskToken& stopToken) const;
};
