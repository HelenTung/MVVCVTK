#include "Preview/CropPreviewPlug.h"

#include <vtkActor.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkShaderProperty.h>
#include <vtkTransform.h>
#include <vtkUniforms.h>
#include <vtkVolume.h>
#include <vtkVolumeMapper.h>

static constexpr const char* kVolumeRemoveInsideEnabledUniform = "mvvcvtk_volumeRemoveInsideEnabled";
static constexpr const char* kVolumeRemoveInsideActiveInputModelToBoxUniform = "mvvcvtk_volumeRemoveInsideActiveInputModelToBox";
static constexpr const char* kVolumeRemoveInsidePlaneEnabledUniform = "mvvcvtk_volumeRemoveInsidePlaneEnabled";
static constexpr const char* kVolumeRemoveInsidePlaneNormalUniform = "mvvcvtk_volumeRemoveInsidePlaneNormal";
static constexpr const char* kVolumeRemoveInsidePlaneOriginUniform = "mvvcvtk_volumeRemoveInsidePlaneOrigin";
static constexpr const char* kVolumeRemoveInsideBaseImplTag = "//VTK::Base::Impl";
static constexpr const char* kPolyDataRemoveInsideActiveInputModelPositionTag = "//VTK::PositionVC::Dec";
static constexpr const char* kPolyDataRemoveInsideActiveInputModelPositionImplTag = "//VTK::PositionVC::Impl";
static constexpr const char* kPolyDataRemoveInsideLightImplTag = "//VTK::Light::Impl";
static constexpr const char* kPolyDataRemoveInsidePolyDataModelToBoxUniform = "mvvcvtk_polyDataRemoveInsidePolyDataModelToBox";
static constexpr const char* kPolyDataRemoveInsidePlaneNormalUniform = "mvvcvtk_polyDataRemoveInsidePlaneNormal";
static constexpr const char* kPolyDataRemoveInsidePlaneOriginUniform = "mvvcvtk_polyDataRemoveInsidePlaneOrigin";

static constexpr const char* kVolumeRemoveInsideBaseImplReplacement =
    "//VTK::Base::Impl\n"
    "    if (!g_skip && (mvvcvtk_volumeRemoveInsideEnabled != 0 || mvvcvtk_volumeRemoveInsidePlaneEnabled != 0))\n"
    "      {\n"
    "      vec4 mvvcvtk_activeInputModelPoint = in_textureDatasetMatrix[0] * vec4(g_dataPos, 1.0);\n"
    "      float mvvcvtk_activeInputModelInvW = abs(mvvcvtk_activeInputModelPoint.w) > 1e-6 ? 1.0 / mvvcvtk_activeInputModelPoint.w : 1.0;\n"
    "      mvvcvtk_activeInputModelPoint = vec4(mvvcvtk_activeInputModelPoint.xyz * mvvcvtk_activeInputModelInvW, 1.0);\n"
    "      if (mvvcvtk_volumeRemoveInsidePlaneEnabled != 0)\n"
    "        {\n"
    "        float mvvcvtk_planeSide = dot(mvvcvtk_activeInputModelPoint.xyz - mvvcvtk_volumeRemoveInsidePlaneOrigin, mvvcvtk_volumeRemoveInsidePlaneNormal);\n"
    "        if (mvvcvtk_planeSide > 0.0)\n"
    "          {\n"
    "          g_skip = true;\n"
    "          }\n"
    "        }\n"
    "      else\n"
    "        {\n"
    "      vec4 mvvcvtk_boxPoint4 = mvvcvtk_volumeRemoveInsideActiveInputModelToBox * mvvcvtk_activeInputModelPoint;\n"
    "      float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "      vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "      if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "        {\n"
    "        g_skip = true;\n"
    "        }\n"
    "        }\n"
    "      }\n";

static constexpr const char* kPolyDataRemoveInsideVertexPositionReplacement =
    "//VTK::PositionVC::Dec\n"
    "out vec4 mvvcvtk_polyDataModelPositionVSOutput;\n";

static constexpr const char* kPolyDataRemoveInsideVertexPositionImplReplacement =
    "//VTK::PositionVC::Impl\n"
    "mvvcvtk_polyDataModelPositionVSOutput = vertexMC;\n";

static constexpr const char* kPolyDataRemoveInsideFragmentPositionReplacement =
    "//VTK::PositionVC::Dec\n"
    "in vec4 mvvcvtk_polyDataModelPositionVSOutput;\n";

static constexpr const char* kPolyDataRemoveInsideLightImplReplacement =
    "//VTK::Light::Impl\n"
    "    vec4 mvvcvtk_polyDataModelPoint = mvvcvtk_polyDataModelPositionVSOutput;\n"
    "    float mvvcvtk_polyDataModelInvW = abs(mvvcvtk_polyDataModelPoint.w) > 1e-6 ? 1.0 / mvvcvtk_polyDataModelPoint.w : 1.0;\n"
    "    mvvcvtk_polyDataModelPoint = vec4(mvvcvtk_polyDataModelPoint.xyz * mvvcvtk_polyDataModelInvW, 1.0);\n"
    "    vec4 mvvcvtk_boxPoint4 = mvvcvtk_polyDataRemoveInsidePolyDataModelToBox * mvvcvtk_polyDataModelPoint;\n"
    "    float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "    vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "    if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "      {\n"
    "      discard;\n"
    "      }\n";

static constexpr const char* kPolyDataRemoveInsidePlaneLightImplReplacement =
    "//VTK::Light::Impl\n"
    "    vec4 mvvcvtk_polyDataModelPoint = mvvcvtk_polyDataModelPositionVSOutput;\n"
    "    float mvvcvtk_polyDataModelInvW = abs(mvvcvtk_polyDataModelPoint.w) > 1e-6 ? 1.0 / mvvcvtk_polyDataModelPoint.w : 1.0;\n"
    "    mvvcvtk_polyDataModelPoint = vec4(mvvcvtk_polyDataModelPoint.xyz * mvvcvtk_polyDataModelInvW, 1.0);\n"
    "    float mvvcvtk_planeSide = dot(mvvcvtk_polyDataModelPoint.xyz - mvvcvtk_polyDataRemoveInsidePlaneOrigin, mvvcvtk_polyDataRemoveInsidePlaneNormal);\n"
    "    if (mvvcvtk_planeSide > 0.0)\n"
    "      {\n"
    "      discard;\n"
    "      }\n";

bool CropPreviewPlug::SetPreview(
    const std::shared_ptr<InteractiveService>& targetService,
    const std::shared_ptr<CropOverlay>& overlayStrategy,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult* volumePreviewResult,
    const OrthogonalCropResult* polyDataPreviewResult,
    CropRemovalMode removalMode)
{
    if (!targetService || !overlayStrategy) {
        return false;
    }

    bool isMainSet = false;
    if (volumePreviewResult) {
        // VolumeData preview 结果在 3D volume 窗口表达 render-only 主预览；
        // 2D mask 由 submit 结果单独进入 overlay，不参与 preview 路由。
        isMainSet = SetVolumeView(
            targetService,
            referenceService,
            *volumePreviewResult,
            removalMode);
    }

    bool isPolySet = false;
    if (!isMainSet && polyDataPreviewResult) {
        // 网格结果只负责 actor 窗口：
        // KeepInside 使用 mapper planes，RemoveInside 使用 actor shader discard。
        isPolySet = SetMeshView(
            targetService,
            referenceService,
            *polyDataPreviewResult,
            removalMode);
        isMainSet = isPolySet;
    }

    overlayStrategy->SetSliceAxis(targetService->GetNavigationAxis());
    overlayStrategy->SetRemovalMode(removalMode);

    const OrthogonalCropResult* overlayResult = volumePreviewResult ? volumePreviewResult : polyDataPreviewResult;
    if (overlayResult) {
        auto visibleOverlayResult = *overlayResult;
        if (isPolySet) {
            // 主 actor 已经用 mapper clipping 或 shader discard 表达 polydata 预览；
            // overlay 是否显示几何参照由 bridge 按 reference 窗口注入；可选裁切网格 artifact 不参与主预览判定。
            visibleOverlayResult.SetClipPolyData(nullptr);
        }
        overlayStrategy->SetCropResult(visibleOverlayResult);
    }
    else {
        overlayStrategy->ClearPreview();
    }

    return isMainSet;
}

void CropPreviewPlug::ResetPreview(
    const std::shared_ptr<InteractiveService>& targetService,
    const std::shared_ptr<CropOverlay>& overlayStrategy)
{
    if (overlayStrategy) {
        overlayStrategy->ClearPreview();
    }

    if (!targetService) {
        return;
    }

    auto volume = vtkVolume::SafeDownCast(targetService->GetMainProp());
    auto volumeMapper = volume ? vtkVolumeMapper::SafeDownCast(volume->GetMapper()) : nullptr;
    if (volumeMapper) {
        ResetVolumeView(volume, volumeMapper);
    }

    ResetMeshView(targetService);
}

vtkSmartPointer<vtkPolyData> CropPreviewPlug::GetPreviewData(
    const std::shared_ptr<InteractiveService>& targetService) const
{
    if (!targetService) {
        return nullptr;
    }

    auto actor = vtkActor::SafeDownCast(targetService->GetMainProp());
    auto mapper = actor ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper()) : nullptr;
    return mapper ? vtkPolyData::SafeDownCast(mapper->GetInput()) : nullptr;
}

void CropPreviewPlug::Clear()
{
    m_targetStates.clear();
}

void CropPreviewPlug::ResetVolumeView(vtkVolume* volume, vtkVolumeMapper* volumeMapper) const
{
    ClearVolumeCut(volume, volumeMapper);
    if (!volume || !volumeMapper) {
        return;
    }

    volumeMapper->RemoveAllClippingPlanes();
    volumeMapper->CroppingOff();
    volumeMapper->SetCroppingRegionFlagsToSubVolume();
    volumeMapper->Modified();
    volume->Modified();
}

void CropPreviewPlug::ClearVolumeCut(vtkVolume* volume, vtkVolumeMapper* volumeMapper) const
{
    if (!volume || !volumeMapper) {
        return;
    }

    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->ClearFragmentShaderReplacement(kVolumeRemoveInsideBaseImplTag, true);
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
    fragmentUniforms->SetUniformi(kVolumeRemoveInsidePlaneEnabledUniform, 0);
    fragmentUniforms->RemoveUniform(kVolumeRemoveInsidePlaneNormalUniform);
    fragmentUniforms->RemoveUniform(kVolumeRemoveInsidePlaneOriginUniform);
    if (auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper)) {
        gpuVolumeMapper->SetMaskInput(nullptr);
    }

    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
}

vtkSmartPointer<vtkPlaneCollection> CropPreviewPlug::BuildBoxClip(
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    const auto& cropData = previewResult.GetCropDataModel();
    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToWorldMat->Identity();
    if (referenceService) {
        inputToWorldMat->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto boxToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMat->DeepCopy(cropData.GetBoxMatrix().data());

    // VTK clipping planes 需要 world 坐标；cropData 保存的是 box -> active input model，
    // 所以先还原 box -> world，再把标准盒 6 个面转换成 world plane。
    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(inputToWorldMat, boxToInputMat, boxToWorldMatrix);

    auto boxToWorldTransform = vtkSmartPointer<vtkTransform>::New();
    boxToWorldTransform->SetMatrix(boxToWorldMatrix);

    auto clippingPlanes = vtkSmartPointer<vtkPlaneCollection>::New();
    for (int axis = 0; axis < 3; ++axis) {
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
            const double side = sideIndex == 0 ? -1.0 : 1.0;
            double boxOrigin[3] = { 0.0, 0.0, 0.0 };
            boxOrigin[axis] = side;

            double boxNormal[3] = { 0.0, 0.0, 0.0 };
            // KeepInside 需要保留 6 个面共同围住的内部区域；
            // 法线朝内时，VTK clipping planes 的交集语义正好表达盒内保留。
            boxNormal[axis] = -side;

            double worldOrigin[3] = { 0.0, 0.0, 0.0 };
            double worldNormal[3] = { 0.0, 0.0, 0.0 };
            boxToWorldTransform->TransformPoint(boxOrigin, worldOrigin);
            boxToWorldTransform->TransformNormal(boxNormal, worldNormal);

            if (vtkMath::Normalize(worldNormal) <= 1e-12) {
                continue;
            }

            auto plane = vtkSmartPointer<vtkPlane>::New();
            plane->SetOrigin(worldOrigin);
            plane->SetNormal(worldNormal);
            clippingPlanes->AddItem(plane);
        }
    }

    return clippingPlanes;
}

vtkSmartPointer<vtkPlaneCollection> CropPreviewPlug::BuildPlaneClip(
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    const auto& cropData = previewResult.GetCropDataModel();
    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToWorldMat->Identity();
    if (referenceService) {
        inputToWorldMat->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto inputToWorld = vtkSmartPointer<vtkTransform>::New();
    inputToWorld->SetMatrix(inputToWorldMat);

    const auto planeCenterInInputModel = cropData.GetPlaneCenter();
    const auto planeNormalInInputModel = cropData.GetPlaneNormal();
    double worldOrigin[3] = { 0.0, 0.0, 0.0 };
    double worldNormal[3] = { 0.0, 0.0, 1.0 };
    inputToWorld->TransformPoint(planeCenterInInputModel.data(), worldOrigin);
    inputToWorld->TransformNormal(planeNormalInInputModel.data(), worldNormal);
    if (vtkMath::Normalize(worldNormal) <= 1e-12) {
        worldNormal[0] = 0.0;
        worldNormal[1] = 0.0;
        worldNormal[2] = 1.0;
    }

    // VTK clipping plane 保留法线指向的一侧（与 box 的“法线朝内保留盒内”一致）。
    // KeepInside 要保留法线正侧，因此直接使用原始法线，不能反转。
    // 本函数只在 KeepInside 路径调用；RemoveInside 由 shader discard 负责保留法线负侧。

    auto plane = vtkSmartPointer<vtkPlane>::New();
    plane->SetOrigin(worldOrigin);
    plane->SetNormal(worldNormal);

    auto clippingPlanes = vtkSmartPointer<vtkPlaneCollection>::New();
    clippingPlanes->AddItem(plane);
    return clippingPlanes;
}

bool CropPreviewPlug::SetVolumeView(
    const std::shared_ptr<InteractiveService>& targetService,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult,
    CropRemovalMode removalMode)
{
    if (!targetService
        || targetService->GetNavigationAxis() >= 0
        || !previewResult.GetSucceeded()
        || previewResult.GetResolvedDataSource() != OrthogonalCropDataSource::VolumeData
        || previewResult.GetResolvedOperation() != OrthogonalCropOperation::Preview) {
        return false;
    }

    auto volume = vtkVolume::SafeDownCast(targetService->GetMainProp());
    auto volumeMapper = volume ? vtkVolumeMapper::SafeDownCast(volume->GetMapper()) : nullptr;
    if (!volumeMapper) {
        return false;
    }

    // 每次接管前先清空上一种 volume 后端表达，避免 KeepInside clipping
    // 与 RemoveInside shader discard / mask 在反复切换时互相残留。
    ClearVolumeCut(volume, volumeMapper);
    volumeMapper->RemoveAllClippingPlanes();
    volumeMapper->CroppingOff();

    if (removalMode == CropRemovalMode::RemoveInside) {
        auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper);
        const bool isPlane = previewResult.GetResolvedGeometryType() == CropShape::Plane;
        const bool isApplied = isPlane
            ? SetVolumePlane(volume, gpuVolumeMapper, previewResult)
            : SetVolumeRemove(volume, gpuVolumeMapper, previewResult);
        if (!isApplied) {
            volumeMapper->SetCroppingRegionFlagsToSubVolume();
            volume->Modified();
            return false;
        }

        volume->Modified();
        return true;
    }

    SetVolumeKeep(volumeMapper, referenceService, previewResult);
    volume->Modified();
    return true;
}

void CropPreviewPlug::SetVolumeKeep(
    vtkVolumeMapper* volumeMapper,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!volumeMapper) {
        return;
    }

    // 体渲染 mapper 的 clipping planes 需要 world 坐标；
    // 结果只提供标准盒到当前输入模型，world 矩阵来自参考窗口当前主数据。
    volumeMapper->SetClippingPlanes(
        previewResult.GetResolvedGeometryType() == CropShape::Plane
            ? BuildPlaneClip(referenceService, previewResult)
            : BuildBoxClip(referenceService, previewResult));
}

bool CropPreviewPlug::SetVolumeRemove(
    vtkVolume* volume,
    vtkGPUVolumeRayCastMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volume || !volumeMapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMat->DeepCopy(cropData.GetBoxMatrix().data());

    // RemoveInside shader 在采样点上判断是否落入标准盒 [-1,1]^3。
    // 采样点先由 VTK shader 内置矩阵还原到 active input model，再送入标准盒空间。
    auto inputToBoxMat = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToInputMat, inputToBoxMat);

    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->AddFragmentShaderReplacement(
        kVolumeRemoveInsideBaseImplTag,
        true,
        kVolumeRemoveInsideBaseImplReplacement,
        false);

    auto inputToBoxShader = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToBoxShader->DeepCopy(inputToBoxMat);
    // 上传前转置矩阵；
    // VTK uniforms 按 OpenGL 列主序解释，转置后 shader 乘法才与 C++ activeInputModelToBox 结果一致。
    inputToBoxShader->Transpose();

    // vtk volume shader 中 g_dataPos 是纹理坐标；
    // in_textureDatasetMatrix[0] 会把它还原到 active input model。
    // vtkUniforms 上传 vtkMatrix4x4 时按 OpenGL 列主序解释，所以自定义 activeInputModelToBox 需要先转置再交给 shader。
    // uniforms 的声明由 VTK 的 CustomUniforms::Dec 自动生成，不能再手写到 Base::Dec，否则会重复声明。
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kVolumeRemoveInsideActiveInputModelToBoxUniform, inputToBoxShader);
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 1);

    // box 与 plane 共用同一段 shader replacement，shader 同时引用 plane uniform。
    // VTK 只在 SetUniform 时生成对应声明，因此 box 路径必须把 plane uniform 也声明出来
    // （enabled=0 + 占位值），否则 shader 编译会报 undefined variable。
    const double inactivePlaneNormal[3] = { 0.0, 0.0, 1.0 };
    const double inactivePlaneOrigin[3] = { 0.0, 0.0, 0.0 };
    fragmentUniforms->SetUniformi(kVolumeRemoveInsidePlaneEnabledUniform, 0);
    fragmentUniforms->SetUniform3f(kVolumeRemoveInsidePlaneNormalUniform, inactivePlaneNormal);
    fragmentUniforms->SetUniform3f(kVolumeRemoveInsidePlaneOriginUniform, inactivePlaneOrigin);

    volumeMapper->SetMaskInput(nullptr);
    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
    return true;
}

bool CropPreviewPlug::SetVolumePlane(
    vtkVolume* volume,
    vtkGPUVolumeRayCastMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volume || !volumeMapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->AddFragmentShaderReplacement(
        kVolumeRemoveInsideBaseImplTag,
        true,
        kVolumeRemoveInsideBaseImplReplacement,
        false);

    const auto planeNormalInInputModel = cropData.GetPlaneNormal();
    const auto planeCenterInInputModel = cropData.GetPlaneCenter();

    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    auto inactiveBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    inactiveBoxMatrix->Identity();
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
    fragmentUniforms->SetUniformMatrix(kVolumeRemoveInsideActiveInputModelToBoxUniform, inactiveBoxMatrix);
    fragmentUniforms->SetUniform3f(kVolumeRemoveInsidePlaneNormalUniform, planeNormalInInputModel.data());
    fragmentUniforms->SetUniform3f(kVolumeRemoveInsidePlaneOriginUniform, planeCenterInInputModel.data());
    fragmentUniforms->SetUniformi(kVolumeRemoveInsidePlaneEnabledUniform, 1);

    volumeMapper->SetMaskInput(nullptr);
    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
    return true;
}

void CropPreviewPlug::ResetMeshView(
    const std::shared_ptr<InteractiveService>& targetService)
{
    auto key = targetService.get();
    auto state = key ? m_targetStates.find(key) : m_targetStates.end();

    if (!targetService) {
        if (state != m_targetStates.end()) {
            state->second.mainPreviewMapper = nullptr;
        }
        return;
    }

    auto actor = vtkActor::SafeDownCast(targetService->GetMainProp());
    if (!actor) {
        if (state != m_targetStates.end()) {
            state->second.mainPreviewMapper = nullptr;
        }
        return;
    }

    auto mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
    if (mapper) {
        mapper->RemoveAllClippingPlanes();
        mapper->Modified();
    }
    if (state != m_targetStates.end()) {
        state->second.mainPreviewMapper = nullptr;
    }

    auto shaderProperty = actor->GetShaderProperty();
    shaderProperty->ClearVertexShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionTag, true);
    shaderProperty->ClearVertexShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionImplTag, true);
    shaderProperty->ClearFragmentShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionTag, true);
    shaderProperty->ClearFragmentShaderReplacement(kPolyDataRemoveInsideLightImplTag, true);
    shaderProperty->GetFragmentCustomUniforms()->RemoveUniform(kPolyDataRemoveInsidePolyDataModelToBoxUniform);
    shaderProperty->GetFragmentCustomUniforms()->RemoveUniform(kPolyDataRemoveInsidePlaneNormalUniform);
    shaderProperty->GetFragmentCustomUniforms()->RemoveUniform(kPolyDataRemoveInsidePlaneOriginUniform);
    shaderProperty->Modified();
    actor->Modified();
}

bool CropPreviewPlug::SetMeshView(
    const std::shared_ptr<InteractiveService>& targetService,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult,
    CropRemovalMode removalMode)
{
    if (!targetService || targetService->GetNavigationAxis() >= 0 || !previewResult.GetSucceeded()) {
        return false;
    }

    auto actor = vtkActor::SafeDownCast(targetService->GetMainProp());
    if (!actor) {
        return false;
    }

    auto mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
    if (!mapper) {
        return false;
    }

    // 每次接管前先清空上一种 polydata 后端表达，避免 KeepInside clipping planes
    // 与 RemoveInside shader discard 在反复切换时互相残留。
    ResetMeshView(targetService);
    auto& targetState = m_targetStates[targetService.get()];
    targetState.mainPreviewMapper = mapper;
    if (removalMode == CropRemovalMode::KeepInside) {
        SetMeshKeep(mapper, referenceService, previewResult);
        actor->Modified();
        return true;
    }

    return previewResult.GetResolvedGeometryType() == CropShape::Plane
        ? SetMeshPlaneCut(actor, mapper, referenceService, previewResult)
        : SetMeshRemove(actor, mapper, referenceService, previewResult);
}

void CropPreviewPlug::SetMeshKeep(
    vtkPolyDataMapper* mapper,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!mapper) {
        return;
    }

    // PolyData KeepInside 与 volume KeepInside 使用同一套 world clipping planes；
    // mapper 会把 world planes 转回当前 actor data 坐标，因此只附加状态也能保留裁切效果。
    mapper->SetClippingPlanes(
        previewResult.GetResolvedGeometryType() == CropShape::Plane
            ? BuildPlaneClip(referenceService, previewResult)
            : BuildBoxClip(referenceService, previewResult));
    mapper->Modified();
}

bool CropPreviewPlug::SetMeshRemove(
    vtkActor* actor,
    vtkPolyDataMapper* mapper,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!actor || !mapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToInputMat = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToInputMat->DeepCopy(cropData.GetBoxMatrix().data());

    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToWorldMat->Identity();
    if (referenceService) {
        inputToWorldMat->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(inputToWorldMat, boxToInputMat, boxToWorldMatrix);

    auto worldToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToWorldMatrix, worldToBoxMatrix);

    auto polyToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    actor->GetModelToWorldMatrix(polyToWorldMat);

    // polydata shader 的输入点是 actor 自己的 model 坐标，不是 volume shader 的 g_dataPos。
    // 因此先把 crop box 提升到 world，再用 actor model->world 折出 polyData model -> box。
    auto polyDataModelToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(worldToBoxMatrix, polyToWorldMat, polyDataModelToBoxMatrix);

    auto shaderProperty = actor->GetShaderProperty();
    shaderProperty->AddVertexShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionTag,
        true,
        kPolyDataRemoveInsideVertexPositionReplacement,
        false);
    shaderProperty->AddVertexShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionImplTag,
        true,
        kPolyDataRemoveInsideVertexPositionImplReplacement,
        false);
    shaderProperty->AddFragmentShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionTag,
        true,
        kPolyDataRemoveInsideFragmentPositionReplacement,
        false);
    shaderProperty->AddFragmentShaderReplacement(
        kPolyDataRemoveInsideLightImplTag,
        true,
        kPolyDataRemoveInsideLightImplReplacement,
        false);

    auto polyToBoxShader = vtkSmartPointer<vtkMatrix4x4>::New();
    polyToBoxShader->DeepCopy(polyDataModelToBoxMatrix);
    polyToBoxShader->Transpose();

    // PolyData RemoveInside 只在标准盒空间判断 [-1, 1]^3；
    // C++ 侧预先折叠 polyData model -> box，并按 VTK shader uniform 约定上传转置矩阵。
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kPolyDataRemoveInsidePolyDataModelToBoxUniform, polyToBoxShader);

    shaderProperty->Modified();
    mapper->Modified();
    actor->Modified();
    return true;
}

bool CropPreviewPlug::SetMeshPlaneCut(
    vtkActor* actor,
    vtkPolyDataMapper* mapper,
    const std::shared_ptr<InteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!actor || !mapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto inputToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    inputToWorldMat->Identity();
    if (referenceService) {
        inputToWorldMat->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto inputToWorld = vtkSmartPointer<vtkTransform>::New();
    inputToWorld->SetMatrix(inputToWorldMat);

    const auto planeCenterInInputModel = cropData.GetPlaneCenter();
    const auto planeNormalInInputModel = cropData.GetPlaneNormal();
    double worldOrigin[3] = { 0.0, 0.0, 0.0 };
    double worldNormal[3] = { 0.0, 0.0, 1.0 };
    inputToWorld->TransformPoint(planeCenterInInputModel.data(), worldOrigin);
    inputToWorld->TransformNormal(planeNormalInInputModel.data(), worldNormal);
    if (vtkMath::Normalize(worldNormal) <= 1e-12) {
        return false;
    }

    auto polyToWorldMat = vtkSmartPointer<vtkMatrix4x4>::New();
    actor->GetModelToWorldMatrix(polyToWorldMat);

    auto worldToPolyMat = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(polyToWorldMat, worldToPolyMat);

    auto worldToPoly = vtkSmartPointer<vtkTransform>::New();
    worldToPoly->SetMatrix(worldToPolyMat);

    double polyDataModelOrigin[3] = { 0.0, 0.0, 0.0 };
    double polyDataModelNormal[3] = { 0.0, 0.0, 1.0 };
    worldToPoly->TransformPoint(worldOrigin, polyDataModelOrigin);
    worldToPoly->TransformNormal(worldNormal, polyDataModelNormal);
    if (vtkMath::Normalize(polyDataModelNormal) <= 1e-12) {
        return false;
    }

    auto shaderProperty = actor->GetShaderProperty();
    shaderProperty->AddVertexShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionTag,
        true,
        kPolyDataRemoveInsideVertexPositionReplacement,
        false);
    shaderProperty->AddVertexShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionImplTag,
        true,
        kPolyDataRemoveInsideVertexPositionImplReplacement,
        false);
    shaderProperty->AddFragmentShaderReplacement(
        kPolyDataRemoveInsideActiveInputModelPositionTag,
        true,
        kPolyDataRemoveInsideFragmentPositionReplacement,
        false);
    shaderProperty->AddFragmentShaderReplacement(
        kPolyDataRemoveInsideLightImplTag,
        true,
        kPolyDataRemoveInsidePlaneLightImplReplacement,
        false);

    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniform3f(kPolyDataRemoveInsidePlaneNormalUniform, polyDataModelNormal);
    fragmentUniforms->SetUniform3f(kPolyDataRemoveInsidePlaneOriginUniform, polyDataModelOrigin);

    shaderProperty->Modified();
    mapper->Modified();
    actor->Modified();
    return true;
}
