// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Routing/CropRouter.cpp
// 分类: Service / Backend Router Implementation
// 说明: 按 request 指定的裁切类型 / 动作 / 数据源，把图像 / 体渲染 / 网格请求直接分发到算法层。
// =====================================================================

#include "Routing/CropRouter.h"

#include "Algorithms/OrthogonalCropAlgorithm.h"
#include "Algorithms/PlanarCropAlgorithm.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef GetMessage
#undef GetMessage
#endif
#endif

#include <memory>
#include <string>
#include <utility>

class CropRouter::Impl final {
public:
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    void ClearInputPolyData();
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);
    OrthogonalCropDataSource GetActiveDataSource() const;
    std::array<double, 6> GetActiveInputModelBounds() const;
    OrthogonalCropRequest GetDefaultRequest() const;
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

private:
    OrthogonalCropResult GetBoxResult(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetPlaneResult(const OrthogonalCropRequest& request) const;
    std::array<double, 6> GetImageModelBounds() const;
    std::array<double, 6> GetPolyBounds() const;
    std::size_t GetRamBytes() const;

    // 当前 image 输入的 VTK 共享 owner；SetInputImage 替换，image/volume 路由读取同一对象。
    vtkSmartPointer<vtkImageData> m_inputImage;
    // 当前 polydata 输入的 VTK 共享 owner；SetInputPolyData 替换，ClearInputPolyData 释放本侧引用。
    vtkSmartPointer<vtkPolyData> m_inputPolyData;
    // 调用方选择的首选数据源；GetActiveDataSource 在对应输入缺失时按现有 image、polydata 顺序回退。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::ImageData;
};

void CropRouter::Impl::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_inputImage = std::move(image);
}

vtkSmartPointer<vtkImageData> CropRouter::Impl::GetInputImage() const
{
    return m_inputImage;
}

void CropRouter::Impl::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_inputPolyData = std::move(polyData);
}

void CropRouter::Impl::ClearInputPolyData()
{
    m_inputPolyData = nullptr;
}

vtkSmartPointer<vtkPolyData> CropRouter::Impl::GetInputPolyData() const
{
    return m_inputPolyData;
}

void CropRouter::Impl::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_preferredDataSource = dataSource;
}

OrthogonalCropDataSource CropRouter::Impl::GetActiveDataSource() const
{
    const bool hasImage = GetInputImage() != nullptr;
    const bool hasPolyData = GetInputPolyData() != nullptr;

    if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::VolumeData && hasImage) {
        return OrthogonalCropDataSource::VolumeData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    if (hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    return m_preferredDataSource;
}

std::array<double, 6> CropRouter::Impl::GetActiveInputModelBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
    case OrthogonalCropDataSource::VolumeData:
        return GetImageModelBounds();
    case OrthogonalCropDataSource::PolyData:
        return GetPolyBounds();
    default:
        return bounds;
    }
}

OrthogonalCropRequest CropRouter::Impl::GetDefaultRequest() const
{
    const auto activeDataSource = GetActiveDataSource();
    OrthogonalCropRequest request;
    request.SetDataSource(activeDataSource);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetBoxBounds(GetActiveInputModelBounds());
    request.SetOperation(OrthogonalCropOperation::Preview);
    return request;
}

OrthogonalCropResult CropRouter::Impl::GetResult(const OrthogonalCropRequest& request) const
{
    // 公开入口只分发裁切类型；动作和数据源留在对应几何的内部路由里，避免多套入口表达同一件事。
    switch (request.GetGeometryType()) {
    case CropShape::Box:
        return GetBoxResult(request);

    case CropShape::Plane:
        return GetPlaneResult(request);

    default:
        return OrthogonalCropResult::GetFailure(
            request,
            CropFailure::NoBackend,
            "Router has no executable path for this crop geometry.");
    }
}

OrthogonalCropResult CropRouter::Impl::GetBoxResult(const OrthogonalCropRequest& request) const
{
    // Box 路由只放行当前三种真实路径：体预览、网格预览、图像提交；其它组合统一停在 router。
    switch (request.GetOperation()) {
    case OrthogonalCropOperation::Preview:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::VolumeData:
            if (!GetInputImage()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoImage,
                    "Image-backed crop route requires image input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetRamBytes());

        case OrthogonalCropDataSource::PolyData:
            if (!GetInputPolyData()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoPolyData,
                    "PolyData crop route requires polydata input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(m_inputPolyData, request);

        default:
            break;
        }
        break;

    case OrthogonalCropOperation::Submit:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            if (!GetInputImage()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoImage,
                    "Image-backed crop route requires image input data.");
            }
            return OrthogonalCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetRamBytes());

        default:
            break;
        }
        break;

    default:
        break;
    }

    return OrthogonalCropResult::GetFailure(
        request,
        CropFailure::NoBackend,
        "Router has no executable path for this crop route.");
}

OrthogonalCropResult CropRouter::Impl::GetPlaneResult(const OrthogonalCropRequest& request) const
{
    // Plane 路由放行与 Box 相同的主链路：体预览、网格预览、图像提交。
    switch (request.GetOperation()) {
    case OrthogonalCropOperation::Preview:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::VolumeData:
            if (!GetInputImage()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoImage,
                    "Image-backed planar crop route requires image input data.");
            }
            return PlanarCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetRamBytes());

        case OrthogonalCropDataSource::PolyData:
            if (!GetInputPolyData()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoPolyData,
                    "PolyData planar crop route requires polydata input data.");
            }
            return PlanarCropAlgorithm::GetResult(m_inputPolyData, request);

        default:
            break;
        }
        break;

    case OrthogonalCropOperation::Submit:
        switch (request.GetDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            if (!GetInputImage()) {
                return OrthogonalCropResult::GetFailure(
                    request,
                    CropFailure::NoImage,
                    "Image-backed planar crop route requires image input data.");
            }
            return PlanarCropAlgorithm::GetResult(
                m_inputImage,
                request,
                GetRamBytes());

        default:
            break;
        }
        break;

    default:
        break;
    }

    return OrthogonalCropResult::GetFailure(
        request,
        CropFailure::NoBackend,
        "Router has no executable path for this planar crop route.");
}

std::array<double, 6> CropRouter::Impl::GetImageModelBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    auto image = GetInputImage();
    if (!image) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    image->GetBounds(rawBounds);
    return {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
}

std::array<double, 6> CropRouter::Impl::GetPolyBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!m_inputPolyData) {
        return bounds;
    }

    double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    m_inputPolyData->GetBounds(rawBounds);
    return {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
}

std::size_t CropRouter::Impl::GetRamBytes() const
{
#ifdef _WIN32
    MEMORYSTATUSEX memoryStatus = {};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) != 0) {
        return static_cast<std::size_t>(memoryStatus.ullAvailPhys);
    }
#endif
    return 0;
}

CropRouter::CropRouter()
    : m_impl(std::make_unique<CropRouter::Impl>())
{
}

CropRouter::~CropRouter() = default;

CropRouter::CropRouter(CropRouter&&) noexcept = default;

CropRouter& CropRouter::operator=(CropRouter&&) noexcept = default;

void CropRouter::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_impl->SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> CropRouter::GetInputImage() const
{
    return m_impl->GetInputImage();
}

void CropRouter::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_impl->SetInputPolyData(std::move(polyData));
}

void CropRouter::ClearInputPolyData()
{
    m_impl->ClearInputPolyData();
}

vtkSmartPointer<vtkPolyData> CropRouter::GetInputPolyData() const
{
    return m_impl->GetInputPolyData();
}

void CropRouter::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_impl->SetPreferredDataSource(dataSource);
}

OrthogonalCropDataSource CropRouter::GetActiveDataSource() const
{
    return m_impl->GetActiveDataSource();
}

std::array<double, 6> CropRouter::GetActiveInputModelBounds() const
{
    return m_impl->GetActiveInputModelBounds();
}

OrthogonalCropRequest CropRouter::GetDefaultRequest() const
{
    return m_impl->GetDefaultRequest();
}

OrthogonalCropResult CropRouter::GetResult(const OrthogonalCropRequest& request) const
{
    return m_impl->GetResult(request);
}
