#include "OrthogonalCropPreviewPlugService.h"

#include <vtkAlgorithm.h>
#include <vtkActor.h>
#include <vtkAlgorithmOutput.h>
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
static constexpr const char* kVolumeRemoveInsideBaseImplTag = "//VTK::Base::Impl";
static constexpr const char* kPolyDataRemoveInsideActiveInputModelPositionTag = "//VTK::PositionVC::Dec";
static constexpr const char* kPolyDataRemoveInsideActiveInputModelPositionImplTag = "//VTK::PositionVC::Impl";
static constexpr const char* kPolyDataRemoveInsideClipImplTag = "//VTK::Clip::Impl";
static constexpr const char* kPolyDataRemoveInsideLightImplTag = "//VTK::Light::Impl";
static constexpr const char* kPolyDataRemoveInsidePolyDataModelToWorldUniform = "mvvcvtk_polyDataRemoveInsidePolyDataModelToWorld";
static constexpr const char* kPolyDataRemoveInsideWorldToBoxUniform = "mvvcvtk_polyDataRemoveInsideWorldToBox";

static constexpr const char* kVolumeRemoveInsideBaseImplReplacement =
    "//VTK::Base::Impl\n"
    "    if (!g_skip && mvvcvtk_volumeRemoveInsideEnabled != 0)\n"
    "      {\n"
    "      vec4 mvvcvtk_activeInputModelPoint = in_textureDatasetMatrix[0] * vec4(g_dataPos, 1.0);\n"
    "      float mvvcvtk_activeInputModelInvW = abs(mvvcvtk_activeInputModelPoint.w) > 1e-6 ? 1.0 / mvvcvtk_activeInputModelPoint.w : 1.0;\n"
    "      mvvcvtk_activeInputModelPoint = vec4(mvvcvtk_activeInputModelPoint.xyz * mvvcvtk_activeInputModelInvW, 1.0);\n"
    "      vec4 mvvcvtk_boxPoint4 = mvvcvtk_volumeRemoveInsideActiveInputModelToBox * mvvcvtk_activeInputModelPoint;\n"
    "      float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "      vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "      if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "        {\n"
    "        g_skip = true;\n"
    "        }\n"
    "      }\n";

static constexpr const char* kPolyDataRemoveInsideVertexPositionReplacement =
    "//VTK::PositionVC::Dec\n"
    "out vec4 mvvcvtk_worldPositionVSOutput;\n";

static constexpr const char* kPolyDataRemoveInsideVertexPositionImplReplacement =
    "//VTK::PositionVC::Impl\n"
    "mvvcvtk_worldPositionVSOutput = mvvcvtk_polyDataRemoveInsidePolyDataModelToWorld * vertexMC;\n";

static constexpr const char* kPolyDataRemoveInsideFragmentPositionReplacement =
    "//VTK::PositionVC::Dec\n"
    "in vec4 mvvcvtk_worldPositionVSOutput;\n";

static constexpr const char* kPolyDataRemoveInsideClipImplReplacement =
    "//VTK::Clip::Impl\n"
    "    vec4 mvvcvtk_worldPoint = mvvcvtk_worldPositionVSOutput;\n"
    "    float mvvcvtk_worldInvW = abs(mvvcvtk_worldPoint.w) > 1e-6 ? 1.0 / mvvcvtk_worldPoint.w : 1.0;\n"
    "    mvvcvtk_worldPoint = vec4(mvvcvtk_worldPoint.xyz * mvvcvtk_worldInvW, 1.0);\n"
    "    vec4 mvvcvtk_boxPoint4 = mvvcvtk_polyDataRemoveInsideWorldToBox * mvvcvtk_worldPoint;\n"
    "    float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "    vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "    if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "      {\n"
    "      discard;\n"
    "      }\n";

static constexpr const char* kPolyDataRemoveInsideLightImplReplacement =
    "//VTK::Light::Impl\n"
    "    vec4 mvvcvtk_worldPoint = mvvcvtk_worldPositionVSOutput;\n"
    "    float mvvcvtk_worldInvW = abs(mvvcvtk_worldPoint.w) > 1e-6 ? 1.0 / mvvcvtk_worldPoint.w : 1.0;\n"
    "    mvvcvtk_worldPoint = vec4(mvvcvtk_worldPoint.xyz * mvvcvtk_worldInvW, 1.0);\n"
    "    vec4 mvvcvtk_boxPoint4 = mvvcvtk_polyDataRemoveInsideWorldToBox * mvvcvtk_worldPoint;\n"
    "    float mvvcvtk_boxInvW = abs(mvvcvtk_boxPoint4.w) > 1e-6 ? 1.0 / mvvcvtk_boxPoint4.w : 1.0;\n"
    "    vec3 mvvcvtk_boxPoint = mvvcvtk_boxPoint4.xyz * mvvcvtk_boxInvW;\n"
    "    if (all(lessThanEqual(abs(mvvcvtk_boxPoint), vec3(1.0))))\n"
    "      {\n"
    "      discard;\n"
    "      }\n";

bool OrthogonalCropPreviewPlugService::ApplyPreview(
    const std::shared_ptr<AbstractInteractiveService>& targetService,
    const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult,
    CropRemovalMode removalMode)
{
    if (!targetService || !overlayStrategy) {
        return false;
    }

    bool mainPreviewApplied = ApplyVolumePreview(targetService, referenceService, previewResult, removalMode);
    if (!mainPreviewApplied) {
        mainPreviewApplied = ApplyPolyDataPreview(targetService, referenceService, previewResult, removalMode);
    }

    overlayStrategy->SetSliceAxis(targetService->GetNavigationAxis());
    overlayStrategy->SetRemovalMode(removalMode);
    overlayStrategy->SetCropResult(previewResult);
    return mainPreviewApplied;
}

void OrthogonalCropPreviewPlugService::RestorePreview(
    const std::shared_ptr<AbstractInteractiveService>& targetService,
    const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy)
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
        volume->GetShaderProperty()->ClearFragmentShaderReplacement(kVolumeRemoveInsideBaseImplTag, true);
        volume->GetShaderProperty()->GetFragmentCustomUniforms()->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
        if (auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper)) {
            gpuVolumeMapper->SetMaskInput(nullptr);
        }
        volumeMapper->RemoveAllClippingPlanes();
        volumeMapper->CroppingOff();
        volumeMapper->SetCroppingRegionFlagsToSubVolume();
        volume->Modified();
    }

    RestorePolyDataPreview(targetService);
}

void OrthogonalCropPreviewPlugService::Clear()
{
    m_targetStates.clear();
}

vtkSmartPointer<vtkPolyData> OrthogonalCropPreviewPlugService::GetPreviewPolyData(
    const std::shared_ptr<AbstractInteractiveService>& targetService) const
{
    if (!targetService || targetService->GetNavigationAxis() >= 0) {
        return nullptr;
    }

    auto actor = vtkActor::SafeDownCast(targetService->GetMainProp());
    auto mapper = actor ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper()) : nullptr;
    if (!mapper) {
        return nullptr;
    }

    vtkPolyData* polyData = nullptr;
    const auto state = m_targetStates.find(targetService.get());
    if (state != m_targetStates.end() && state->second.mainPreviewMapper == mapper) {
        if (state->second.mainPreviewInputConnection
            && state->second.mainPreviewInputConnection->GetProducer()) {
            auto producer = state->second.mainPreviewInputConnection->GetProducer();
            producer->Update();
            polyData = vtkPolyData::SafeDownCast(
                producer->GetOutputDataObject(state->second.mainPreviewInputConnection->GetIndex()));
        }
        else {
            polyData = state->second.mainPreviewInputData;
        }
    }
    else {
        mapper->Update();
        polyData = mapper->GetInput();
    }

    if (!polyData || polyData->GetNumberOfPoints() <= 0) {
        return nullptr;
    }

    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->ShallowCopy(polyData);
    return output;
}

vtkSmartPointer<vtkPlaneCollection> OrthogonalCropPreviewPlugService::BuildWorldClippingPlanes(
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    const auto& cropData = previewResult.GetCropDataModel();
    auto activeInputModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToWorldMatrix->Identity();
    if (referenceService) {
        activeInputModelToWorldMatrix->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto boxToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToActiveInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    // VTK clipping planes 需要 world 坐标；cropData 保存的是 box -> active input model，
    // 所以先还原 box -> world，再把标准盒 6 个面转换成 world plane。
    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(activeInputModelToWorldMatrix, boxToActiveInputModelMatrix, boxToWorldMatrix);

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

bool OrthogonalCropPreviewPlugService::ApplyVolumePreview(
    const std::shared_ptr<AbstractInteractiveService>& targetService,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult,
    CropRemovalMode removalMode)
{
    if (!targetService || targetService->GetNavigationAxis() >= 0 || !previewResult.GetSucceeded()) {
        return false;
    }

    auto volume = vtkVolume::SafeDownCast(targetService->GetMainProp());
    auto volumeMapper = volume ? vtkVolumeMapper::SafeDownCast(volume->GetMapper()) : nullptr;
    if (!volumeMapper) {
        return false;
    }

    // 每次接管前先清空上一种 volume 后端表达，避免 KeepInside clipping
    // 与 RemoveInside shader discard / mask 在反复切换时互相残留。
    volume->GetShaderProperty()->ClearFragmentShaderReplacement(kVolumeRemoveInsideBaseImplTag, true);
    volume->GetShaderProperty()->GetFragmentCustomUniforms()->SetUniformi(kVolumeRemoveInsideEnabledUniform, 0);
    if (auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper)) {
        gpuVolumeMapper->SetMaskInput(nullptr);
    }
    volumeMapper->RemoveAllClippingPlanes();
    volumeMapper->CroppingOff();

    if (removalMode != CropRemovalMode::KeepInside) {
        auto gpuVolumeMapper = vtkGPUVolumeRayCastMapper::SafeDownCast(volumeMapper);
        if (!ApplyVolumeRemoveInsidePreview(volume, gpuVolumeMapper, previewResult)) {
            volumeMapper->SetCroppingRegionFlagsToSubVolume();
            volume->Modified();
            return false;
        }

        volume->Modified();
        return true;
    }

    ApplyVolumeKeepInsidePreview(volumeMapper, referenceService, previewResult);
    volume->Modified();
    return true;
}

void OrthogonalCropPreviewPlugService::ApplyVolumeKeepInsidePreview(
    vtkVolumeMapper* volumeMapper,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!volumeMapper) {
        return;
    }

    // KeepInside 和 RemoveInside 都只消费 previewResult.cropData；
    // 这里仅在 VTK volume clipping 需要的后端表达点，把 box/active input model 盒还原成 world planes。
    volumeMapper->SetClippingPlanes(BuildWorldClippingPlanes(referenceService, previewResult));
}

bool OrthogonalCropPreviewPlugService::ApplyVolumeRemoveInsidePreview(
    vtkVolume* volume,
    vtkGPUVolumeRayCastMapper* volumeMapper,
    const OrthogonalCropResult& previewResult) const
{
    if (!volume || !volumeMapper) {
        return false;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToActiveInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    // RemoveInside shader 在采样点上判断是否落入标准盒 [-1,1]^3。
    // 采样点先由 VTK shader 内置矩阵还原到 active input model，再用 activeInputModelToBox 送入标准盒空间。
    auto activeInputModelToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToActiveInputModelMatrix, activeInputModelToBoxMatrix);

    auto shaderProperty = volume->GetShaderProperty();
    shaderProperty->AddFragmentShaderReplacement(
        kVolumeRemoveInsideBaseImplTag,
        true,
        kVolumeRemoveInsideBaseImplReplacement,
        false);

    // 体渲染只消费 previewResult 中的 cropData；业务来源统一由 request -> cropData 决定。
    auto activeInputModelToBoxShaderMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToBoxShaderMatrix->DeepCopy(activeInputModelToBoxMatrix);
    // 上传前转置矩阵；
    // VTK uniforms 按 OpenGL 列主序解释，转置后 shader 乘法才与 C++ activeInputModelToBox 结果一致。
    activeInputModelToBoxShaderMatrix->Transpose();

    // vtk volume shader 中 g_dataPos 是纹理坐标；
    // in_textureDatasetMatrix[0] 会把它还原到 active input model。
    // vtkUniforms 上传 vtkMatrix4x4 时按 OpenGL 列主序解释，所以自定义 activeInputModelToBox 需要先转置再交给 shader。
    // uniforms 的声明由 VTK 的 CustomUniforms::Dec 自动生成，不能再手写到 Base::Dec，否则会重复声明。
    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kVolumeRemoveInsideActiveInputModelToBoxUniform, activeInputModelToBoxShaderMatrix);
    fragmentUniforms->SetUniformi(kVolumeRemoveInsideEnabledUniform, 1);

    volumeMapper->SetMaskInput(nullptr);
    shaderProperty->Modified();
    volumeMapper->Modified();
    volume->Modified();
    return true;
}

void OrthogonalCropPreviewPlugService::RestorePolyDataPreview(
    const std::shared_ptr<AbstractInteractiveService>& targetService)
{
    auto key = targetService.get();
    auto state = key ? m_targetStates.find(key) : m_targetStates.end();
    if (state != m_targetStates.end() && state->second.mainPreviewMapper) {
        auto mapper = state->second.mainPreviewMapper;
        mapper->RemoveAllClippingPlanes();
        if (state->second.mainPreviewInputConnection) {
            mapper->SetInputConnection(state->second.mainPreviewInputConnection);
        }
        else if (state->second.mainPreviewInputData) {
            mapper->SetInputData(state->second.mainPreviewInputData);
        }
        mapper->Modified();
        state->second.mainPreviewMapper = nullptr;
        state->second.mainPreviewInputConnection = nullptr;
        state->second.mainPreviewInputData = nullptr;
    }

    if (!targetService) {
        return;
    }

    auto actor = vtkActor::SafeDownCast(targetService->GetMainProp());
    if (!actor) {
        return;
    }

    auto shaderProperty = actor->GetShaderProperty();
    shaderProperty->ClearVertexShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionTag, true);
    shaderProperty->ClearVertexShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionImplTag, true);
    shaderProperty->ClearFragmentShaderReplacement(kPolyDataRemoveInsideActiveInputModelPositionTag, true);
    shaderProperty->ClearFragmentShaderReplacement(kPolyDataRemoveInsideClipImplTag, true);
    shaderProperty->ClearFragmentShaderReplacement(kPolyDataRemoveInsideLightImplTag, true);
    shaderProperty->GetVertexCustomUniforms()->RemoveUniform(kPolyDataRemoveInsidePolyDataModelToWorldUniform);
    shaderProperty->GetFragmentCustomUniforms()->RemoveUniform(kPolyDataRemoveInsideWorldToBoxUniform);
    shaderProperty->Modified();
    actor->Modified();
}

bool OrthogonalCropPreviewPlugService::ApplyPolyDataPreview(
    const std::shared_ptr<AbstractInteractiveService>& targetService,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
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
    RestorePolyDataPreview(targetService);
    auto& targetState = m_targetStates[targetService.get()];
    targetState.mainPreviewMapper = mapper;
    targetState.mainPreviewInputConnection = mapper->GetInputConnection(0, 0);
    targetState.mainPreviewInputData = mapper->GetInput();

    if (removalMode == CropRemovalMode::KeepInside) {
        ApplyPolyDataKeepInsidePreview(mapper, referenceService, previewResult);
        actor->Modified();
        return true;
    }

    return ApplyPolyDataRemoveInsidePreview(actor, mapper, referenceService, previewResult);
}

void OrthogonalCropPreviewPlugService::ApplyPolyDataKeepInsidePreview(
    vtkPolyDataMapper* mapper,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!mapper) {
        return;
    }

    // PolyData KeepInside 与 volume KeepInside 使用同一套 world clipping planes；
    // mapper 会把 world planes 转回当前 actor data 坐标，因此等值面策略重绑 input 后仍能保留裁切状态。
    mapper->SetClippingPlanes(BuildWorldClippingPlanes(referenceService, previewResult));
    mapper->Modified();
}

bool OrthogonalCropPreviewPlugService::ApplyPolyDataRemoveInsidePreview(
    vtkActor* actor,
    vtkPolyDataMapper* mapper,
    const std::shared_ptr<AbstractInteractiveService>& referenceService,
    const OrthogonalCropResult& previewResult) const
{
    if (!actor || !mapper) {
        return false;
    }

    auto clippedPolyData = previewResult.GetClipPolyData();
    if (clippedPolyData && clippedPolyData->GetNumberOfPoints() > 0) {
        mapper->SetInputData(clippedPolyData);
        mapper->Modified();
        actor->Modified();
        return true;
    }

    const auto& cropData = previewResult.GetCropDataModel();
    auto boxToActiveInputModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    boxToActiveInputModelMatrix->DeepCopy(cropData.GetBoxToInputModelMatrix().data());

    auto activeInputModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    activeInputModelToWorldMatrix->Identity();
    if (referenceService) {
        activeInputModelToWorldMatrix->DeepCopy(referenceService->GetModelMatrix().data());
    }

    auto boxToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(activeInputModelToWorldMatrix, boxToActiveInputModelMatrix, boxToWorldMatrix);

    auto worldToBoxMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(boxToWorldMatrix, worldToBoxMatrix);

    auto polyDataModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    actor->GetModelToWorldMatrix(polyDataModelToWorldMatrix);

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
        kPolyDataRemoveInsideClipImplTag,
        true,
        kPolyDataRemoveInsideClipImplReplacement,
        false);
    shaderProperty->AddFragmentShaderReplacement(
        kPolyDataRemoveInsideLightImplTag,
        true,
        kPolyDataRemoveInsideLightImplReplacement,
        false);

    auto worldToBoxShaderMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToBoxShaderMatrix->DeepCopy(worldToBoxMatrix);
    auto polyDataModelToWorldShaderMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    polyDataModelToWorldShaderMatrix->DeepCopy(polyDataModelToWorldMatrix);
    // 上传前转置矩阵；
    // VTK uniforms 按 OpenGL 列主序解释，转置后 shader 乘法才与 C++ 矩阵结果一致。
    worldToBoxShaderMatrix->Transpose();
    polyDataModelToWorldShaderMatrix->Transpose();

    // PolyData RemoveInside 在 world 空间判断当前片元是否落入标准盒；
    // 这样 image 派生的等值面、直接输入的 polydata，以及红色预览框共享同一显示坐标。
    auto vertexUniforms = shaderProperty->GetVertexCustomUniforms();
    vertexUniforms->SetUniformMatrix(kPolyDataRemoveInsidePolyDataModelToWorldUniform, polyDataModelToWorldShaderMatrix);

    auto fragmentUniforms = shaderProperty->GetFragmentCustomUniforms();
    fragmentUniforms->SetUniformMatrix(kPolyDataRemoveInsideWorldToBoxUniform, worldToBoxShaderMatrix);

    shaderProperty->Modified();
    mapper->Modified();
    actor->Modified();
    return true;
}
