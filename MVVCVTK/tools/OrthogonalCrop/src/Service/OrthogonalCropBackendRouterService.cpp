// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/OrthogonalCropBackendRouterService.cpp
// 分类: Service / Backend Router Implementation
// 说明: 在 image plugin 与 polydata clip 之间统一分发请求，并回填统一结果模型。
// =====================================================================
// 这里不重新发明裁切算法：
// - image 输入沿用 algorithm/plugin 的 mask / extract 语义
// - polydata 输入只负责把同一份 cropData 翻译成 implicit function 后做 clip
// - 两条路径最后都折叠成统一结果模型，供上层按同一种方式读取

#include "OrthogonalCropBackendRouterService.h"

#include <vtkBox.h>
#include <vtkGeometryFilter.h>
#include <vtkTableBasedClipDataSet.h>
#include <vtkTransform.h>

#include <cmath>
#include <cstddef>
#include <utility>

// image 输入由 image-only plugin 持有；
// router 只负责在 image 与 polydata 后端之间做统一分发。
void OrthogonalCropBackendRouterService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_imageService.SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> OrthogonalCropBackendRouterService::GetInputImage() const
{
    return m_imageService.GetInputImage();
}

// polydata 输入直接缓存在 router；
// 输入变化会使旧 clip 结果失效，因此必须同步清空缓存。
void OrthogonalCropBackendRouterService::SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
{
    m_inputPolyData = std::move(polyData);
    m_cachedPolyDataInput = nullptr;
    m_cachedClippedPolyData = nullptr;
    m_hasCachedPolyDataClip = false;
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetInputPolyData() const
{
    return m_inputPolyData;
}

// 数据源偏好设置：影响 GetActiveDataSource() 的路由优先级
void OrthogonalCropBackendRouterService::SetPreferredDataSource(OrthogonalCropDataSource dataSource)
{
    m_preferredDataSource = dataSource;
}

OrthogonalCropDataSource OrthogonalCropBackendRouterService::GetActiveDataSource() const
{
    // 先尊重显式 preferredDataSource；若对应输入不存在，再按当前可用输入回退。
    const bool hasImage = GetInputImage() != nullptr;
    const bool hasPolyData = GetInputPolyData() != nullptr;

    if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasImage) {
        return OrthogonalCropDataSource::ImageData;
    }

    if (hasPolyData) {
        return OrthogonalCropDataSource::PolyData;
    }

    return OrthogonalCropDataSource::Auto;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetActiveInputModelBounds() const
{
    std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        return GetImageModelBounds();
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataInputModelBounds();
    default:
        return bounds;
    }
}

OrthogonalCropRequest OrthogonalCropBackendRouterService::GetDefaultRequest() const
{
    if (GetActiveDataSource() == OrthogonalCropDataSource::ImageData) {
        return m_imageService.GetDefaultRequest();
    }

    // polydata 路径没有 image 的 spacing/origin 语义，因此默认 request 直接围绕输入 bounds 构造。
    OrthogonalCropRequest request;
    request.SetExecutionMode(CropExecutionMode::PreviewArtifact);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
    request.SetBoxToInputModelMatrixFromBounds(GetActiveInputModelBounds());
    return request;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetStatistics(const OrthogonalCropRequest& request) const
{
    // 诊断入口与执行入口保持同样的路由规则；
    // PreviewArtifact 的产物粒度也在这里生效，避免轻量请求误触发重型 clip / mask 诊断。
    if (request.GetExecutionMode() == CropExecutionMode::PreviewArtifact
        && request.GetPreviewArtifactMode() == CropPreviewArtifactMode::Lightweight3DOutlineGuide) {
        const auto guideResult = GetGuidePreviewResult(request);
        OrthogonalCropStatistics statistics;
        statistics.SetResolvedDataSource(guideResult.GetResolvedDataSource());
        statistics.SetResolvedBackend(guideResult.GetResolvedBackend());
        statistics.SetFailureReason(guideResult.GetFailureReason());
        statistics.SetValidationMessage(guideResult.GetMessage());
        return statistics;
    }

    if (request.GetExecutionMode() == CropExecutionMode::Submit) {
        if (!GetInputImage()) {
            auto statistics = GetMissingInputStatistics();
            statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);
            statistics.SetValidationMessage("Image submit requires image input data.");
            return statistics;
        }

        auto statistics = m_imageService.GetStatistics(request);
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);
        return statistics;
    }

    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData: {
        auto statistics = m_imageService.GetStatistics(request);
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::MaskPreview);
        return statistics;
    }
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataStatistics(request);
    default:
        return GetMissingInputStatistics();
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(const OrthogonalCropRequest& request) const
{
    // 根据执行模式、预览产物粒度和活跃数据源路由后端；
    // bridge 只提交统一 request，具体 guide / mask / clip / submit 入口都在 Router 内收口。
    if (request.GetExecutionMode() == CropExecutionMode::PreviewArtifact
        && request.GetPreviewArtifactMode() == CropPreviewArtifactMode::Lightweight3DOutlineGuide) {
        return GetGuidePreviewResult(request);
    }

    if (request.GetExecutionMode() == CropExecutionMode::Submit) {
        if (!GetInputImage()) {
            auto result = GetMissingInputResult();
            result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);
            result.SetRemovalMode(request.GetRemovalMode());
            result.SetMessage("Image submit requires image input data.");
            return result;
        }

        auto result = m_imageService.GetResult(request);
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        result.SetRemovalMode(request.GetRemovalMode());
        if (result.GetResolvedBackend() == OrthogonalCropResolvedBackend::None) {
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::SubmitExtractVOI);
        }
        return result;
    }

    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData: {
        // image 分支委托 PluginService；
        // Router 只补 resolvedDataSource，保持 image 算法入口独立。
        auto result = m_imageService.GetResult(request);
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        result.SetRemovalMode(request.GetRemovalMode());
        if (request.GetExecutionMode() == CropExecutionMode::PreviewArtifact
            && result.GetResolvedBackend() == OrthogonalCropResolvedBackend::None) {
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::MaskPreview);
        }
        return result;
    }
    case OrthogonalCropDataSource::PolyData:
        // polydata 分支在 Router 内完成归一化、clip 和结果封装；
        // 缓存判断留在 GetClippedPolyData，避免重复执行昂贵 VTK 管道。
        return GetPolyDataResult(request);
    default: {
        auto result = GetMissingInputResult();
        result.SetRemovalMode(request.GetRemovalMode());
        return result;
    }
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetGuidePreviewResult(const OrthogonalCropRequest& request) const
{
    OrthogonalCropResult result;
    result.SetCropStateModel(request.GetCropStateModel());
    result.SetRemovalMode(request.GetRemovalMode());

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    const auto activeDataSource = GetActiveDataSource();
    switch (activeDataSource) {
    case OrthogonalCropDataSource::ImageData:
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::None);
        if (!OrthogonalCropAlgorithm::GetCropDataModel(
                GetImageModelBounds(),
                request,
                cropData,
                failureReason,
                message,
                true)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(message);
            return result;
        }
        break;
    case OrthogonalCropDataSource::PolyData:
        result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::None);
        if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(message);
            return result;
        }
        break;
    default:
        return GetMissingInputResult();
    }

    // 3D outline guide 只提供显示分发必需字段；诊断、2D mask 和 3D clipped polydata 留给完整结果入口。
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(result.GetResolvedDataSource());
    statistics.SetResolvedBackend(result.GetResolvedBackend());
    statistics.SetFailureReason(OrthogonalCropFailureReason::None);

    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    result.SetStatistics(statistics);
    result.SetCropDataModel(cropData);
    result.SetOutlinePolyData(OrthogonalCropAlgorithm::GetOutlinePolyData(cropData));
    return result;
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetClippedPolyData(
    const CropDataModel& cropData,
    CropRemovalMode removalMode) const
{
    if (!m_inputPolyData) {
        return nullptr;
    }

    // polydata clip 缓存必须同时匹配输入、removal mode 和几何状态；
    // 任一条件变化都重新执行 VTK 管道，避免复用过期裁切结果。
    bool canReuseCachedClip = m_hasCachedPolyDataClip
        && m_cachedPolyDataInput == m_inputPolyData.GetPointer()
        && m_cachedPolyDataRemovalMode == removalMode
        && m_cachedClippedPolyData;
    // input model bounds 的细微变化也会影响 clip 结果；
    // 用固定 epsilon 比较 6 个边界值，避免尺寸变化后继续复用旧输出。
    if (canReuseCachedClip) {
        constexpr double epsilon = 1e-9;
        const auto& cachedInputModelBounds = m_cachedPolyDataCropData.GetInputModelBounds();
        const auto& inputModelBounds = cropData.GetInputModelBounds();
        for (std::size_t index = 0; index < inputModelBounds.size(); ++index) {
            if (std::abs(cachedInputModelBounds[index] - inputModelBounds[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
    }

    // boxToInputModelMatrix 是有向盒姿态真源；
    // 旋转、平移或缩放任一变化都会触发重裁切。
    if (canReuseCachedClip) {
        constexpr double epsilon = 1e-9;
        const auto& cachedBoxToInputModelMatrix = m_cachedPolyDataCropData.GetBoxToInputModelMatrix();
        const auto& boxToInputModelMatrixData = cropData.GetBoxToInputModelMatrix();
        for (std::size_t index = 0; index < boxToInputModelMatrixData.size(); ++index) {
            if (std::abs(cachedBoxToInputModelMatrix[index] - boxToInputModelMatrixData[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
    }

    // 缓存命中时直接复用 clipped polydata；
    // 这是 polydata preview 避免重复跑 VTK clip 管道的主要性能保护。
    if (canReuseCachedClip) {
        return m_cachedClippedPolyData;
    }

    // 缓存未命中时构造标准盒隐函数；
    // cropData 已归一化，activeInputModelToBox transform 负责把 active input model 点送回 [-1,1]^3。
    auto clipFunction = vtkSmartPointer<vtkBox>::New();
    const auto canonicalBounds = GetCanonicalCropBoxBounds();
    clipFunction->SetBounds(
        canonicalBounds[0], canonicalBounds[1],
        canonicalBounds[2], canonicalBounds[3],
        canonicalBounds[4], canonicalBounds[5]);

    auto activeInputModelToBoxTransform = vtkSmartPointer<vtkTransform>::New();
    activeInputModelToBoxTransform->SetMatrix(cropData.GetBoxToInputModelMatrix().data());
    activeInputModelToBoxTransform->Inverse();
    clipFunction->SetTransform(activeInputModelToBoxTransform);

    // VTK clip 管道按需创建并在 Router 生命周期内复用；
    // 后续 preview 只替换输入和 clip function，减少对象重建成本。
    if (!m_polyDataClipFilter) {
        m_polyDataClipFilter = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
    }
    // vtkGeometryFilter 统一 clip 输出为 polydata；
    // 下游渲染器和 ShallowCopy 因此不需要处理多种 VTK 数据集类型。
    if (!m_polyDataGeometryFilter) {
        m_polyDataGeometryFilter = vtkSmartPointer<vtkGeometryFilter>::New();
        m_polyDataGeometryFilter->SetInputConnection(m_polyDataClipFilter->GetOutputPort());
    }

    // 执行当前 cropData 对应的 clip；
    // InsideOut 只由 removal mode 决定，保持 KeepInside/RemoveInside 语义集中。
    m_polyDataClipFilter->SetInputData(m_inputPolyData);
    m_polyDataClipFilter->SetClipFunction(clipFunction);
    if (removalMode == CropRemovalMode::KeepInside) {
        m_polyDataClipFilter->InsideOutOn();
    }
    else {
        m_polyDataClipFilter->InsideOutOff();
    }
    m_polyDataGeometryFilter->Update();

    // ShallowCopy 当前输出作为稳定 result；
    // 既隔离后续管道更新，又避免不必要的深拷贝内存成本。
    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(m_polyDataGeometryFilter->GetOutput());

    // 更新缓存键和值；
    // 下一次相同输入和几何状态可以直接复用本次 clip 结果。
    m_cachedPolyDataCropData = cropData;
    m_cachedPolyDataRemovalMode = removalMode;
    m_cachedPolyDataInput = m_inputPolyData.GetPointer();
    m_cachedClippedPolyData = output;
    m_hasCachedPolyDataClip = true;

    return output;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetImageModelBounds() const
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

std::array<double, 6> OrthogonalCropBackendRouterService::GetPolyDataInputModelBounds() const
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

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetMissingInputStatistics() const
{
    OrthogonalCropStatistics statistics;
    statistics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
    statistics.SetValidationMessage("No active crop input data is bound.");
    return statistics;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetMissingInputResult() const
{
    OrthogonalCropResult result;
    result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
    result.SetMessage("No active crop input data is bound.");
    return result;
}

bool OrthogonalCropBackendRouterService::GetPolyDataCropDataModel(
    const OrthogonalCropRequest& request,
    CropDataModel& cropData,
    OrthogonalCropFailureReason& failureReason,
    std::string& message) const
{
    if (!m_inputPolyData) {
        failureReason = OrthogonalCropFailureReason::InputPolyDataMissing;
        message = "Input polydata is null.";
        return false;
    }

    // polydata 路径复用 Algorithm 的 request 归一化逻辑
    // allowPartialOverlap=true: 盒子只要和输入有交集即可执行 clip (不要求完全包含)
    return OrthogonalCropAlgorithm::GetCropDataModel(
        GetPolyDataInputModelBounds(),
        request,
        cropData,
        failureReason,
        message,
        true);
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ClipPreview);

    if (!m_inputPolyData) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        statistics.SetValidationMessage("Input polydata is null.");
        return statistics;
    }

    if (!clipped) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        statistics.SetValidationMessage("Failed to build clipped polydata.");
        return statistics;
    }

    statistics.SetFailureReason(OrthogonalCropFailureReason::None);
    return statistics;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetPolyDataStatistics(const OrthogonalCropRequest& request) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ClipPreview);

    if (!m_inputPolyData) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        statistics.SetValidationMessage("Input polydata is null.");
        return statistics;
    }

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
        statistics.SetFailureReason(failureReason);
        statistics.SetValidationMessage(message);
        return statistics;
    }

    auto clipped = GetClippedPolyData(cropData, request.GetRemovalMode());
    if (!clipped) {
        statistics.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        statistics.SetValidationMessage("Failed to build clipped polydata.");
        return statistics;
    }

    // 直接复用真实 clipped 结果诊断，避免诊断与执行走不同 polydata clip 结果
    return GetPolyDataStatisticsFromClipped(clipped);
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetPolyDataResult(const OrthogonalCropRequest& request) const
{
    // polydata 结果与 image preview 使用同一份 result contract；
    // preview 层因此只消费 diagnostics、cropData、outline 和 artifact，不需要知道后端分支。
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::ClipPreview);
    result.SetRemovalMode(request.GetRemovalMode());
    result.SetCropStateModel(request.GetCropStateModel());

    // 先把 request 归一化为 polydata cropData；
    // preview 允许部分重叠，用户拖出边界时仍能看到有效 clip 反馈。
    if (!m_inputPolyData) {
        result.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
        result.SetMessage("Input polydata is null.");
        return result;
    }

    CropDataModel cropData;
    OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
    std::string message;
    if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
        result.SetFailureReason(failureReason);
        result.SetMessage(message);
        return result;
    }

    // 执行或复用 polydata clip；
    // 缓存键包含 input、input model bounds 和 boxToInputModelMatrix，避免重复跑相同 VTK 管道。
    auto clipped = GetClippedPolyData(cropData, request.GetRemovalMode());
    if (!clipped) {
        result.SetFailureReason(OrthogonalCropFailureReason::ClipPreviewPolyDataCreationFailed);
        result.SetMessage("Failed to build clipped polydata.");
        return result;
    }

    // 回填 diagnostics、cropData、outline 和 clipped polydata；
    // result 形状保持与 image preview 一致，方便上层统一分发。
    const auto statistics = GetPolyDataStatisticsFromClipped(clipped);

    result.SetStatistics(statistics);
    result.SetCropDataModel(cropData);
    result.SetOutlinePolyData(OrthogonalCropAlgorithm::GetOutlinePolyData(cropData));
    result.SetClipPolyData(clipped);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}
