#pragma once

#include "OrthogonalCrop/OrthogonalCropPluginService.h"

#include <vtkBox.h>
#include <vtkDoubleArray.h>
#include <vtkGeometryFilter.h>
#include <vtkImplicitFunction.h>
#include <vtkMatrix4x4.h>
#include <vtkPlanes.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkTableBasedClipDataSet.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

// 在 image-only 插件和 polydata clip 之间做统一路由的桥接服务。
class OrthogonalCropBackendRouterService {
public:
    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image)
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

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
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

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        m_preferredDataSource = dataSource;
    }

    // 保留面向使用方的简写 setter。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        CropPreInit_SetPreferredDataSource(dataSource);
    }

    // 优先尊重显式偏好，其次按当前已有输入回退到可执行后端。
    OrthogonalCropDataSource GetActiveDataSource() const
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
    std::array<double, 6> GetActiveInputBounds() const
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
    OrthogonalCropRequest GetDefaultRequest() const
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
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const
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
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const
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

    // polydata 路径统一采用 VTK implicit function，再经 geometry filter 输出最终 polydata。
    // 这里不关心上游是 box、planes 还是 cylinder function，保持后端入口可复用。
    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        vtkImplicitFunction* clipFunction,
        CropRemovalMode removalMode)
    {
        if (!polyData || !clipFunction) {
            return nullptr;
        }

        auto clip = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
        clip->SetInputData(polyData);
        clip->SetClipFunction(clipFunction);
        if (removalMode == CropRemovalMode::KeepInside) {
            clip->InsideOutOn();
        }
        else {
            clip->InsideOutOff();
        }
        clip->Update();

        auto geometry = vtkSmartPointer<vtkGeometryFilter>::New();
        geometry->SetInputConnection(clip->GetOutputPort());
        geometry->Update();

        auto output = vtkSmartPointer<vtkPolyData>::New();
        output->ShallowCopy(geometry->GetOutput());
        return output;
    }

    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        const CropDataModel& cropData,
        CropRemovalMode removalMode)
    {
        return GetClippedPolyData(polyData, GetClipFunction(cropData), removalMode);
    }

protected:
    static std::array<double, 3> GetPointTransformed(
        const std::array<double, 16>& matrix,
        const std::array<double, 3>& point)
    {
        const double x = matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3];
        const double y = matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7];
        const double z = matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11];
        const double w = matrix[12] * point[0] + matrix[13] * point[1] + matrix[14] * point[2] + matrix[15];
        const double invW = std::abs(w) > 1e-12 ? 1.0 / w : 1.0;
        return { x * invW, y * invW, z * invW };
    }

    static std::array<double, 3> GetNormalTransformed(
        const std::array<double, 16>& modelToLocalMatrix,
        const std::array<double, 3>& localNormal)
    {
        std::array<double, 3> normal = {
            modelToLocalMatrix[0] * localNormal[0] + modelToLocalMatrix[4] * localNormal[1] + modelToLocalMatrix[8] * localNormal[2],
            modelToLocalMatrix[1] * localNormal[0] + modelToLocalMatrix[5] * localNormal[1] + modelToLocalMatrix[9] * localNormal[2],
            modelToLocalMatrix[2] * localNormal[0] + modelToLocalMatrix[6] * localNormal[1] + modelToLocalMatrix[10] * localNormal[2]
        };
        const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (length > 1e-12) {
            normal[0] /= length;
            normal[1] /= length;
            normal[2] /= length;
        }
        return normal;
    }

    static std::array<double, 16> GetMatrixInverted(const std::array<double, 16>& matrixData)
    {
        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                matrix->SetElement(row, col, matrixData[row * 4 + col]);
            }
        }
        matrix->Invert();

        std::array<double, 16> inverted = { 0.0 };
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                inverted[row * 4 + col] = matrix->GetElement(row, col);
            }
        }
        return inverted;
    }

    static vtkSmartPointer<vtkImplicitFunction> GetClipFunction(const CropDataModel& cropData)
    {
        if (!cropData.GetLocalAlignmentEnabled()) {
            auto box = vtkSmartPointer<vtkBox>::New();
            const auto rasBounds = cropData.GetRasBounds();
            box->SetBounds(
                rasBounds[0], rasBounds[1],
                rasBounds[2], rasBounds[3],
                rasBounds[4], rasBounds[5]);
            return box;
        }

        auto planes = vtkSmartPointer<vtkPlanes>::New();
        auto points = vtkSmartPointer<vtkPoints>::New();
        auto normals = vtkSmartPointer<vtkDoubleArray>::New();
        points->SetNumberOfPoints(6);
        normals->SetNumberOfComponents(3);
        normals->SetNumberOfTuples(6);

        const auto& localToModel = cropData.GetLocalAlignmentMatrix();
        const auto modelToLocal = GetMatrixInverted(localToModel);
        const auto localCenter = cropData.GetLocalCenter();
        const auto localDimensions = cropData.GetLocalDimensions();

        int planeIndex = 0;
        for (int axis = 0; axis < 3; ++axis) {
            for (int signIndex = 0; signIndex < 2; ++signIndex) {
                const double sign = signIndex == 0 ? -1.0 : 1.0;
                auto localPoint = localCenter;
                localPoint[axis] += sign * localDimensions[axis] * 0.5;

                std::array<double, 3> localNormal = { 0.0, 0.0, 0.0 };
                localNormal[axis] = sign;

                const auto modelPoint = GetPointTransformed(localToModel, localPoint);
                const auto modelNormal = GetNormalTransformed(modelToLocal, localNormal);
                points->SetPoint(planeIndex, modelPoint[0], modelPoint[1], modelPoint[2]);
                normals->SetTuple3(planeIndex, modelNormal[0], modelNormal[1], modelNormal[2]);
                ++planeIndex;
            }
        }

        planes->SetPoints(points);
        planes->SetNormals(normals);
        return planes;
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

    // polydata 路径用一次 clip 结果同时回填统计，避免 GetResult 里重复执行昂贵裁切。
    OrthogonalCropStatistics GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const
    {
        OrthogonalCropStatistics statistics;
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

        if (!m_inputPolyData) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
            statistics.SetValidationMessage("Input polydata is null.");
            return statistics;
        }

        if (!clipped) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
            statistics.SetValidationMessage("Failed to build clipped polydata.");
            return statistics;
        }

        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        statistics.SetTotalVoxelCount(static_cast<std::size_t>(m_inputPolyData->GetNumberOfCells()));
        statistics.SetInsideVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetOutputVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetCanExecutePhysicalCrop(true);
        return statistics;
    }

    // image 统计复用原 image-only 插件，再补 resolved source/backend 信息。
    OrthogonalCropStatistics GetImageStatistics(const OrthogonalCropRequest& request) const
    {
        // 先复用 image-only 插件已有统计结果。
        auto statistics = m_imageService.GetStatistics(request);
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        // preview 一律走 virtual mask；物理裁切才标记为真正的 VOI 抽取。
        if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
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

        return GetPolyDataStatisticsFromClipped(clipped);
    }

    // image 完整结果直接沿用 image-only 插件，避免 router 再生成第二份派生数据。
    OrthogonalCropResult GetImageResult(const OrthogonalCropRequest& request) const
    {
        auto result = m_imageService.GetResult(request);
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop
            && result.GetResolvedBackend() == OrthogonalCropResolvedBackend::None) {
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
        }
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

        // 输入缺失时直接返回失败结果，不进入后续归一化和 clip。
        if (!m_inputPolyData) {
            result.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
            result.SetMessage("Input polydata is null.");
            return result;
        }

        // 归一化一次 cropData，用于结果中的 outline、statistics 和最终 clipped polydata 构建。
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

        const auto statistics = GetPolyDataStatisticsFromClipped(clipped);

        // 成功后把几何数据、轮廓和派生 polydata 全部挂回 result。
        result.SetStatistics(statistics);
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
