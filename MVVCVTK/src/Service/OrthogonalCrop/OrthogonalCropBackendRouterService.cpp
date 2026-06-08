// =====================================================================
// Path: MVVCVTK/src/Service/OrthogonalCrop/OrthogonalCropBackendRouterService.cpp
// 分类: Service / Backend Router Implementation
// 说明: 在 image plugin 与 polydata clip 之间统一分发请求，并回填统一结果模型。
// =====================================================================
// 这里不重新发明裁切算法：
// - image 输入沿用 algorithm/plugin 的 mask / extract 语义
// - polydata 输入只负责把同一份 cropData 翻译成 implicit function 后做 clip
// - 两条路径最后都折叠成统一结果模型，供上层按同一种方式读取

#include "OrthogonalCrop/OrthogonalCropBackendRouterService.h"

#include <vtkBox.h>
#include <vtkGeometryFilter.h>
#include <vtkTableBasedClipDataSet.h>
#include <vtkTransform.h>

#include <cmath>
#include <cstddef>
#include <utility>

// ═══ 输入绑定 ═══
// Image输入：委托给 plugin service 的 SetInputImage
void OrthogonalCropBackendRouterService::SetInputImage(vtkSmartPointer<vtkImageData> image)
{
    m_imageService.SetInputImage(std::move(image));
}

vtkSmartPointer<vtkImageData> OrthogonalCropBackendRouterService::GetInputImage() const
{
    return m_imageService.GetInputImage();
}

// PolyData输入：直接存储并清空 clip 缓存
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
    const bool hasPolyData = m_inputPolyData != nullptr;

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

std::array<double, 6> OrthogonalCropBackendRouterService::GetActiveInputBounds() const
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

OrthogonalCropRequest OrthogonalCropBackendRouterService::GetDefaultRequest() const
{
    if (GetActiveDataSource() == OrthogonalCropDataSource::ImageData) {
        return m_imageService.GetDefaultRequest();
    }

    // polydata 路径没有 image 的 spacing/origin 语义，因此默认 request 直接围绕输入 bounds 构造。
    OrthogonalCropRequest request;
    request.SetBoundsMode(CropBoundsMode::InputVolumeBounds);
    request.SetExecutionMode(CropExecutionMode::VirtualCrop);
    request.SetRemovalMode(CropRemovalMode::KeepInside);
    request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
    request.SetLocalToInputMatrix(GetIdentityMatrixArray());
    request.SetLocalCenter({ 0.0, 0.0, 0.0 });

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

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetStatistics(const OrthogonalCropRequest& request) const
{
    // 统计入口与执行入口保持同样的路由规则
    // 避免"能统计但不能执行"来自不同后端的歧义
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        return GetImageStatistics(request);
    case OrthogonalCropDataSource::PolyData:
        return GetPolyDataStatistics(request);
    default:
        return GetMissingInputStatistics();
    }
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetResult(const OrthogonalCropRequest& request) const
{
    // ── 分支入口：根据活跃数据源路由到对应后端 ──
    // 先通过 GetActiveDataSource() 确定后端：
    //   优先 preferredDataSource → 有输入则走对应分支
    //   Auto 模式按 image → polydata 优先级自动回退
    switch (GetActiveDataSource()) {
    case OrthogonalCropDataSource::ImageData:
        // Image 分支 → PluginService → Algorithm
        // Router 只回填 resolvedDataSource 标记
        return GetImageResult(request);
    case OrthogonalCropDataSource::PolyData:
        // PolyData 分支 → Router 内部三步：归一化→clip→结果封装
        // 含 5 层缓存判断避免重复执行昂贵 VTK 管道
        return GetPolyDataResult(request);
    default:
        return GetMissingInputResult();
    }
}

vtkSmartPointer<vtkPolyData> OrthogonalCropBackendRouterService::GetClippedPolyData(
    const CropDataModel& cropData,
    CropRemovalMode removalMode) const
{
    if (!m_inputPolyData) {
        return nullptr;
    }

    // ═══ PolyData 裁切缓存判断：5 层条件，任一不满足则重新执行 ═══
    // 第1层-快速：输入指针 + removalMode + 基本有效性、canReuseCachedClip是缓存标志是否变换标志位
    bool canReuseCachedClip = m_hasCachedPolyDataClip
        && m_cachedPolyDataInput == m_inputPolyData.GetPointer()
        && m_cachedPolyDataRemovalMode == removalMode
        && m_cachedClippedPolyData;
    // 同时检查 LocalAlignmentEnable 开关是否变化，变换了就进去，没变化就不进去
    if (canReuseCachedClip
        && m_cachedPolyDataCropData.GetLocalAlignmentEnabled() != cropData.GetLocalAlignmentEnabled()) {
        canReuseCachedClip = false;
    }

    // 第2层-RasBounds：6个值逐元素比较 (ε=1e-9)，尺寸微调也触发重裁切，边界盒看看有无变化
    if (canReuseCachedClip) {
        constexpr double epsilon = 1e-9;
        const auto& cachedRasBounds = m_cachedPolyDataCropData.GetRasBounds();
        const auto& rasBounds = cropData.GetRasBounds();
        for (std::size_t index = 0; index < rasBounds.size(); ++index) {
            if (std::abs(cachedRasBounds[index] - rasBounds[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
    }

    // 第3层-LocalAlignment：启用时验证 localCenter + localDimensions + alignmentMatrix
    if (canReuseCachedClip && cropData.GetLocalAlignmentEnabled()) {
        constexpr double epsilon = 1e-9;
		// 局部中心点 (3个值)，局部中心看是否有变化，阈值为1e-9，微调也触发重裁切
        const auto& cachedLocalCenter = m_cachedPolyDataCropData.GetLocalCenter();
        const auto& localCenter = cropData.GetLocalCenter();
        for (std::size_t index = 0; index < localCenter.size(); ++index) {
            if (std::abs(cachedLocalCenter[index] - localCenter[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
		// 看看三维局部尺寸有无变化，局部尺寸看是否有变化，阈值为1e-9，微调也触发重裁切
        const auto& cachedLocalDimensions = m_cachedPolyDataCropData.GetLocalDimensions();
        const auto& localDimensions = cropData.GetLocalDimensions();
        for (std::size_t index = 0; canReuseCachedClip && index < localDimensions.size(); ++index) {
            if (std::abs(cachedLocalDimensions[index] - localDimensions[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
		// 最后看看 localToInputMatrix 有无变化，16个值逐元素比较，阈值为1e-9，微调也触发重裁切
        const auto& cachedLocalToInputMatrix = m_cachedPolyDataCropData.GetLocalToInputMatrix();
        const auto& localToInputMatrix = cropData.GetLocalToInputMatrix();
        for (std::size_t index = 0; canReuseCachedClip && index < localToInputMatrix.size(); ++index) {
            if (std::abs(cachedLocalToInputMatrix[index] - localToInputMatrix[index]) > epsilon) {
                canReuseCachedClip = false;
                break;
            }
        }
    }

    // ═══ 缓存命中 === 跳过昂贵的 VTK 管道执行，直接复用
    if (canReuseCachedClip) {
        return m_cachedClippedPolyData;
    }

    // ═══ 缓存未命中 === 构造 vtkBox 隐函数
    //   此函数接收的是 CropDataModel (已在 GetPolyDataCropDataModel 中归一化)
    //   此时 BoundsMode 已被折叠消除, CropDataModel 内部只保留两种几何表达:
    //     LocalAlignmentEnabled=false → cropData.GetRasBounds() [输入空间AABB]
    //     LocalAlignmentEnabled=true  → localCenter+localDimensions+alignmentMatrix
    //   所以这里只需判断 LocalAlignmentEnabled 即可选择正确的 vtkBox 构造路径
    auto clipFunction = vtkSmartPointer<vtkBox>::New();
    if (!cropData.GetLocalAlignmentEnabled()) {
        // 轴对齐模式：直接用后端输入空间 bounds
        const auto rasBounds = cropData.GetRasBounds();
        clipFunction->SetBounds(
            rasBounds[0], rasBounds[1],
            rasBounds[2], rasBounds[3],
            rasBounds[4], rasBounds[5]);
    }
    else {
        const auto localCenter = cropData.GetLocalCenter();
        const auto localDimensions = cropData.GetLocalDimensions();

        // vtkImplicitFunction 会先把输入点变换到隐函数空间，因此这里传入 localToModel 的逆，
        // 就能直接复用 vtkBox 表达“局部对齐盒 + 模型空间输入”的组合语义。
        clipFunction->SetBounds(
            localCenter[0] - localDimensions[0] * 0.5,
            localCenter[0] + localDimensions[0] * 0.5,
            localCenter[1] - localDimensions[1] * 0.5,
            localCenter[1] + localDimensions[1] * 0.5,
            localCenter[2] - localDimensions[2] * 0.5,
            localCenter[2] + localDimensions[2] * 0.5);

        auto modelToLocalTransform = vtkSmartPointer<vtkTransform>::New();
		modelToLocalTransform->SetMatrix(cropData.GetLocalToInputMatrix().data());  // 先设置 localToInput 矩阵（localToModel），再求逆得到 modelToLocal
        modelToLocalTransform->Inverse(); // 求逆变换 local->model 变换 model->local
        clipFunction->SetTransform(modelToLocalTransform);
    }

    // ═══ 懒初始化 VTK 裁切管道: TableBasedClipDataSet → GeometryFilter → PolyData
    // 整个 Router 生命周期只创建一次，后续调用只更换输入
    if (!m_polyDataClipFilter) {
        m_polyDataClipFilter = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
    }
    // VTK 的 clip 过滤器设计为"只做布尔运算"，输出格式取决于输入单元类型与裁切面的交点情况。
    // vtkGeometryFilter 是标准后处理，保证下游（渲染器、ShallowCopy）拿到干净的三角形网格
    if (!m_polyDataGeometryFilter) {
        m_polyDataGeometryFilter = vtkSmartPointer<vtkGeometryFilter>::New();
        m_polyDataGeometryFilter->SetInputConnection(m_polyDataClipFilter->GetOutputPort());
    }

    // ═══ 执行裁切 ===
    m_polyDataClipFilter->SetInputData(m_inputPolyData);
    m_polyDataClipFilter->SetClipFunction(clipFunction);
    if (removalMode == CropRemovalMode::KeepInside) {
        m_polyDataClipFilter->InsideOutOn();   // KeepInside: 保留盒内部
    }
    else {
        m_polyDataClipFilter->InsideOutOff();  // RemoveInside: 保留盒外部
    }
    m_polyDataGeometryFilter->Update();  // 触发整条管道计算

    // ═══ 提取输出: ShallowCopy 共享底层数据以减少内存开销 ===
    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(m_polyDataGeometryFilter->GetOutput());

    // ═══ 更新缓存 tuple: cropData+removalMode+inputPtr+clippedResult ===
    m_cachedPolyDataCropData = cropData;
    m_cachedPolyDataRemovalMode = removalMode;
    m_cachedPolyDataInput = m_inputPolyData.GetPointer();
    m_cachedClippedPolyData = output;
    m_hasCachedPolyDataClip = true;

    return output;
}

std::array<double, 6> OrthogonalCropBackendRouterService::GetImageBounds() const
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

std::array<double, 6> OrthogonalCropBackendRouterService::GetPolyDataBounds() const
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
        GetPolyDataBounds(),
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

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetImageStatistics(const OrthogonalCropRequest& request) const
{
    // image 路径的真实统计仍由 plugin/algorithm 计算；router 只负责把 resolved 字段补完整。
    auto statistics = m_imageService.GetStatistics(request);
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    }
    else {
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
    }

    return statistics;
}

OrthogonalCropStatistics OrthogonalCropBackendRouterService::GetPolyDataStatistics(const OrthogonalCropRequest& request) const
{
    OrthogonalCropStatistics statistics;
    statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

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
        statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
        statistics.SetValidationMessage("Failed to build clipped polydata.");
        return statistics;
    }

    // 直接复用真实 clipped 结果统计，避免额外发明 polydata 估算规则
    return GetPolyDataStatisticsFromClipped(clipped);
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetImageResult(const OrthogonalCropRequest& request) const
{
    // ── 请求转发：Router → PluginService → Algorithm ──
    // PluginService 注入系统可用 RAM (GlobalMemoryStatusEx)
    // Algorithm 完成 cropData 归一化 → virtual mask 或 physical extract
    auto result = m_imageService.GetResult(request);
    // ── 结果回填（Router 层）：只补标记字段 ──
    // resolvedDataSource = ImageData（上游可据此知道数据来源）
    // VirtualCrop 场景下若 Algorithm 未设置 backend，兜底为 ImageVirtualMask
    result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
    if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop
        && result.GetResolvedBackend() == OrthogonalCropResolvedBackend::None) {
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
    }
    return result;
}

OrthogonalCropResult OrthogonalCropBackendRouterService::GetPolyDataResult(const OrthogonalCropRequest& request) const
{
    // ═══ PolyData 分支结果组装 ═══
    // 三步流程: 归一化 → clip 执行 → 结果封装
    // 封装字段: statistics + cropData + outline + derivedPolyData
    // 格式与 Image 路径完全一致, Preview 层统一消费
    OrthogonalCropResult result;
    result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
    result.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);
    result.SetCropStateModel(request.GetCropStateModel());

    // ── 步骤 1：请求归一化 ──
    // GetPolyDataCropDataModel → Algorithm::GetCropDataModel(inputBounds, request)
    // 四种 bounds mode → CropDataModel，allowPartialOverlap=true
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

    // ── 步骤 2：裁切执行 ──
    // GetClippedPolyData 内部含 5 层缓存判断（inputPtr / rasBounds / localParams / alignmentMatrix）
    // 缓存未命中时构建 vtkBox → TableBasedClipDataSet → GeometryFilter 管道
    auto clipped = GetClippedPolyData(cropData, request.GetRemovalMode());
    if (!clipped) {
        result.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
        result.SetMessage("Failed to build clipped polydata.");
        return result;
    }

    // ── 步骤 3：结果回填 ──
    // statistics + cropData + outline + derivedPolyData 一次性封装
    // 格式与 Image 路径完全一致，Preview 层统一消费
    const auto statistics = GetPolyDataStatisticsFromClipped(clipped);

    result.SetStatistics(statistics);
    result.SetCropDataModel(cropData);
    result.SetOutlinePolyData(OrthogonalCropAlgorithm::GetOutlinePolyData(cropData));
    result.SetDerivedPolyData(clipped);
    result.SetSucceeded(true);
    result.SetFailureReason(OrthogonalCropFailureReason::None);
    return result;
}
