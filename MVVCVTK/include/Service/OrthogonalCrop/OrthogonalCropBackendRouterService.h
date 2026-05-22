#pragma once

#include "OrthogonalCrop/OrthogonalCropPluginService.h"

#include <vtkBox.h>
#include <vtkExtractVOI.h>
#include <vtkGeometryFilter.h>
#include <vtkPolyData.h>
#include <vtkTableBasedClipDataSet.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

// Router 层对 bridge 暴露统一后端接口，屏蔽 image/polydata 的实现差异。
class IOrthogonalCropBackendService {
public:
    virtual ~IOrthogonalCropBackendService() = default;

    // 绑定 image 输入，供 image backend 做 virtual/physical crop。
    virtual void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image) = 0;
    // 绑定 polydata 输入，供 polydata backend 做 box clip。
    virtual void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData) = 0;
    // 显式声明优先数据源；Auto 则由 router 按当前输入自行判断。
    virtual void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource) = 0;
    // 返回当前真正可用的后端数据源。
    virtual OrthogonalCropDataSource GetActiveDataSource() const = 0;
    // 返回当前活跃输入对象的原始 bounds。
    virtual std::array<double, 6> GetActiveInputBounds() const = 0;
    // 生成一份后端默认请求，供上层继续填充交互态字段。
    virtual OrthogonalCropRequest GetDefaultRequest() const = 0;
    // 只查询统计信息，不保证带出 derived data。
    virtual OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const = 0;
    // 返回一次完整执行结果，必要时附带 derived image/polydata。
    virtual OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const = 0;
};

// 在 image-only 插件和 polydata clip 之间做统一路由的桥接服务。
class OrthogonalCropBackendRouterService : public IOrthogonalCropBackendService {
public:
    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image) override
    {
        m_imageService.SetInputImage(std::move(image));
    }

    // 对外保留简写命名，减少接入代码噪声。
    void SetInputImage(vtkSmartPointer<vtkImageData> image)
    {
        CropPreInit_SetInputImage(std::move(image));
    }

    // 读取当前 image backend 绑定的输入对象。
    vtkSmartPointer<vtkImageData> GetInputImage() const
    {
        return m_imageService.GetInputImage();
    }

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData) override
    {
        m_inputPolyData = std::move(polyData);
    }

    // 对外保留简写命名，行为与 PreInit 接口一致。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
    {
        CropPreInit_SetInputPolyData(std::move(polyData));
    }

    // 读取当前 polydata 输入对象。
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const
    {
        return m_inputPolyData;
    }

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource) override
    {
        m_preferredDataSource = dataSource;
    }

    // 保留面向使用方的简写 setter。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        CropPreInit_SetPreferredDataSource(dataSource);
    }

    // 优先尊重显式偏好，其次按当前已有输入回退到可执行后端。
    OrthogonalCropDataSource GetActiveDataSource() const override
    {
        const bool hasImage = GetInputImage() != nullptr;
        const bool hasPolyData = m_inputPolyData != nullptr;

        // 第一优先级：若外部显式声明偏好，且对应输入真实存在，就尊重该偏好。
        if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
            return OrthogonalCropDataSource::PolyData;
        }

        if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
            return OrthogonalCropDataSource::ImageData;
        }

        // 第二优先级：Auto 模式下优先复用 image 路径，因为它天然兼容体数据主流程。
        if (hasImage) {
            return OrthogonalCropDataSource::ImageData;
        }

        // image 不存在时才回退到 polydata 路径。
        if (hasPolyData) {
            return OrthogonalCropDataSource::PolyData;
        }

        return OrthogonalCropDataSource::Auto;
    }

    // 对上层隐藏 image/polydata 分支，统一返回当前活跃输入的 bounds。
    std::array<double, 6> GetActiveInputBounds() const override
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageBounds();
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataBounds();
        default:
            return bounds;
        }
    }

    // image 路径复用原插件默认请求；polydata 路径在这里补齐一份等价默认值。
    OrthogonalCropRequest GetDefaultRequest() const override
    {
        if (GetActiveDataSource() == OrthogonalCropDataSource::ImageData) {
            // image 路径直接沿用原插件默认 request，保持既有行为一致。
            return m_imageService.GetDefaultRequest();
        }

        // polydata 路径没有现成默认 request，需要在 router 层补齐一套中性初值。
        OrthogonalCropRequest request;
        request.SetBoundsMode(CropBoundsMode::InputVolumeBounds);
        request.SetExecutionMode(CropExecutionMode::VirtualCrop);
        request.SetRemovalMode(CropRemovalMode::KeepInside);
        request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
        request.SetLocalAlignmentMatrix(GetIdentityMatrixArray());
        request.SetLocalCenter({ 0.0, 0.0, 0.0 });

        // 再从当前活跃输入推导出默认 bounds/center/dimensions，保证上层拿到 request 就可直接覆盖细节字段。
        const auto bounds = GetActiveInputBounds();
        request.SetRasBounds(bounds);
        request.SetCenter({
            (bounds[0] + bounds[1]) * 0.5,
            (bounds[2] + bounds[3]) * 0.5,
            (bounds[4] + bounds[5]) * 0.5
        });
        request.SetDimensions({
            bounds[1] - bounds[0],
            bounds[3] - bounds[2],
            bounds[5] - bounds[4]
        });
        request.SetLocalDimensions(request.GetDimensions());
        return request;
    }

    // 统计查询只负责选择后端，不附带额外 UI 语义。
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const override
    {
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageStatistics(request);
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataStatistics(request);
        default:
            return GetMissingInputStatistics();
        }
    }

    // 完整结果查询会补齐 resolved source/backend，并在需要时生成 derived data。
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const override
    {
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageResult(request);
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataResult(request);
        default:
            return GetMissingInputResult();
        }
    }

protected:
    // KeepInside preview 或 physical crop 都需要真正抽取一块 VOI。
    static vtkSmartPointer<vtkImageData> GetExtractedImage(
        vtkImageData* image,
        const std::array<int, 6>& ijkBounds)
    {
        if (!image) {
            return nullptr;
        }

        // 先通过 vtkExtractVOI 从原体数据中截出目标子块。
        auto extract = vtkSmartPointer<vtkExtractVOI>::New();
        extract->SetInputData(image);
        extract->SetVOI(
            ijkBounds[0], ijkBounds[1],
            ijkBounds[2], ijkBounds[3],
            ijkBounds[4], ijkBounds[5]);
        extract->Update();

        // 再 shallow copy 一份稳定输出，避免调用方继续依赖 filter 生命周期。
        auto output = vtkSmartPointer<vtkImageData>::New();
        output->ShallowCopy(extract->GetOutput());
        return output;
    }

    // polydata 路径统一采用 box clip，再经 geometry filter 输出最终 polydata。
    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        const CropDataModel& cropData,
        CropRemovalMode removalMode)
    {
        if (!polyData) {
            return nullptr;
        }

        // 第一步：把 cropData 的 RAS bounds 组装成 clip function 用的 vtkBox。
        auto box = vtkSmartPointer<vtkBox>::New();
        const auto rasBounds = cropData.GetRasBounds();
        box->SetBounds(
            rasBounds[0], rasBounds[1],
            rasBounds[2], rasBounds[3],
            rasBounds[4], rasBounds[5]);

        // 第二步：对输入 dataset 执行 box clip，并按 removal mode 切换 inside/outside 语义。
        auto clip = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
        clip->SetInputData(polyData);
        clip->SetClipFunction(box);
        if (removalMode == CropRemovalMode::KeepInside) {
            clip->InsideOutOn();
        }
        else {
            clip->InsideOutOff();
        }
        clip->Update();

        // 第三步：clip 输出可能仍是 dataset，这里统一过一层 geometry filter 得到 polydata。
        auto geometry = vtkSmartPointer<vtkGeometryFilter>::New();
        geometry->SetInputConnection(clip->GetOutputPort());
        geometry->Update();

        // 最后 shallow copy 一份结果，避免依赖中间 filter 生命周期。
        auto output = vtkSmartPointer<vtkPolyData>::New();
        output->ShallowCopy(geometry->GetOutput());
        return output;
    }

private:
    // image 输入的原始 bounds 读取 helper。
    std::array<double, 6> GetImageBounds() const
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

    // polydata 输入的原始 bounds 读取 helper。
    std::array<double, 6> GetPolyDataBounds() const
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

    // 没有任何输入数据时返回统一错误统计。
    OrthogonalCropStatistics GetMissingInputStatistics() const
    {
        OrthogonalCropStatistics statistics;
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        statistics.SetValidationMessage("No active crop input data is bound.");
        return statistics;
    }

    // 没有任何输入数据时返回统一错误结果。
    OrthogonalCropResult GetMissingInputResult() const
    {
        OrthogonalCropResult result;
        result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        result.SetMessage("No active crop input data is bound.");
        return result;
    }

    // polydata 在执行前先复用算法层归一化请求，得到标准 cropData。
    bool GetPolyDataCropDataModel(
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message) const
    {
        // 先做最直接的输入空指针保护，避免后面算法层拿空 bounds 工作。
        if (!m_inputPolyData) {
            failureReason = OrthogonalCropFailureReason::InputPolyDataMissing;
            message = "Input polydata is null.";
            return false;
        }

        // 再统一复用算法层的 request -> cropData 归一化逻辑，避免 image/polydata 两条路径各写一套 bounds 解析。
        return OrthogonalCropAlgorithm::GetCropDataModel(
            GetPolyDataBounds(),
            request,
            cropData,
            failureReason,
            message);
    }

    // image 统计复用原 image-only 插件，再补 resolved source/backend 信息。
    OrthogonalCropStatistics GetImageStatistics(const OrthogonalCropRequest& request) const
    {
        // 先复用 image-only 插件已有统计结果。
        auto statistics = m_imageService.GetStatistics(request);
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        // 再根据执行模式和 removal mode 回填这次真正采用的 image backend。
        if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop
            && request.GetRemovalMode() == CropRemovalMode::KeepInside) {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        }
        else if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
        }
        else {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        }

        return statistics;
    }

    // polydata 统计通过 clip 后 cell 数量近似 inside/output 规模。
    OrthogonalCropStatistics GetPolyDataStatistics(const OrthogonalCropRequest& request) const
    {
        // 先写入 polydata 路径的固定元信息，便于上层日志直接读取。
        OrthogonalCropStatistics statistics;
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

        // 输入缺失时直接返回失败统计，不进入后续归一化和 clip。
        if (!m_inputPolyData) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
            statistics.SetValidationMessage("Input polydata is null.");
            return statistics;
        }

        // 先把 request 归一化成标准 cropData，后面的统计和结果构建都基于这份统一数据模型。
        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
            statistics.SetFailureReason(failureReason);
            statistics.SetValidationMessage(message);
            return statistics;
        }

        // 再实际做一次 clip，统计 inside/output 规模。
        auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
        if (!clipped) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
            statistics.SetValidationMessage("Failed to build clipped polydata.");
            return statistics;
        }

        // polydata 路径没有 voxel 概念，这里统一用 cell 数量近似表达 total/inside/output。
        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        statistics.SetTotalVoxelCount(static_cast<std::size_t>(m_inputPolyData->GetNumberOfCells()));
        statistics.SetInsideVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetOutputVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetCanExecutePhysicalCrop(true);
        return statistics;
    }

    // image 完整结果优先沿用原插件，仅在需要 extracted preview 时额外构建 derived image。
    OrthogonalCropResult GetImageResult(const OrthogonalCropRequest& request) const
    {
        // 第一步：先拿 image-only 插件现有结果，复用其 bounds 解析和统计逻辑。
        auto result = m_imageService.GetResult(request);
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);

        // 插件已经失败时，只补 resolved backend，不再继续派生 derived image。
        if (!result.GetSucceeded()) {
            if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
                result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
            }
            return result;
        }

        // 只有 physical crop 或 keep-inside 预览才需要真正抽取一份 image 子块。
        const bool useExtractPreview = request.GetExecutionMode() == CropExecutionMode::PhysicalCrop
            || request.GetRemovalMode() == CropRemovalMode::KeepInside;
        if (!useExtractPreview) {
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
            return result;
        }

        // 进入抽取路径前再次校验输入 image，避免 result/statistics 正常但原图像已丢失。
        auto image = GetInputImage();
        if (!image) {
            result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
            result.SetMessage("Input image is null.");
            result.SetSucceeded(false);
            return result;
        }

        // 用统计阶段已经 snap 好的 IJK bounds 提取真正的 derived image。
        const auto snappedIjkBounds = result.GetStatistics().GetSnappedIjkBounds();
        auto extractedImage = GetExtractedImage(image, snappedIjkBounds);
        if (!extractedImage) {
            result.SetFailureReason(OrthogonalCropFailureReason::DerivedImageCreationFailed);
            result.SetMessage("Failed to build extracted preview image.");
            result.SetSucceeded(false);
            return result;
        }

        // 最后把派生出的 image 挂回 result，并标记 resolved backend。
        result.SetDerivedImage(extractedImage);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        return result;
    }

    // polydata 完整结果由 router 自行组装，包括统计、outline 和 clipped polydata。
    OrthogonalCropResult GetPolyDataResult(const OrthogonalCropRequest& request) const
    {
        // 先写入 polydata 路径的固定元信息和本次请求携带的 crop state。
        OrthogonalCropResult result;
        result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);
        result.SetCropStateModel(request.GetCropStateModel());

        // 先复用统计路径，统一处理输入缺失、bounds 非法等前置失败。
        const auto statistics = GetPolyDataStatistics(request);
        result.SetStatistics(statistics);
        result.SetFailureReason(statistics.GetFailureReason());
        if (statistics.GetFailureReason() != OrthogonalCropFailureReason::None) {
            result.SetMessage(statistics.GetValidationMessage());
            return result;
        }

        // 再归一化一次 cropData，用于结果中的 outline 和最终 clipped polydata 构建。
        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(message);
            return result;
        }

        // 实际执行 box clip，得到最终输出 polydata。
        auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
        if (!clipped) {
            result.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
            result.SetMessage("Failed to build clipped polydata.");
            return result;
        }

        // 成功后把几何数据、轮廓和派生 polydata 全部挂回 result。
        result.SetCropDataModel(cropData);
        result.SetOutlinePolyData(OrthogonalCropInternal::GetOutlinePolyData(cropData));
        result.SetDerivedPolyData(clipped);
        result.SetSucceeded(true);
        result.SetFailureReason(OrthogonalCropFailureReason::None);
        return result;
    }

    // image 路径复用现有 image-only 裁切服务。
    OrthogonalCropPluginService m_imageService;
    // polydata 输入单独缓存在 router 内。
    vtkSmartPointer<vtkPolyData> m_inputPolyData;
    // 外部声明的后端偏好；Auto 表示按当前输入自动判定。
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
