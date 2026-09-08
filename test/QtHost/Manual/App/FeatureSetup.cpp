// 测试用途：按构建开关创建并挂载七项功能，组合手动和自动化共用的测试页面。
#include "FeatureSetup.h"
#include "Modules/ModuleFactories.h"
#include "Support/ReferenceDataSource.h"
#if defined(MANUAL_CROP)
#include "Host/CropHostFeature.h"
#endif
#if defined(MANUAL_GAP)
#include "Host/GapHostFeature.h"
#endif
#if defined(MANUAL_PART)
#include "Host/PartSegmentationHostFeature.h"
#endif
#if defined(MANUAL_SURFACE)
#include "Host/SurfaceDeterminationHostFeature.h"
#endif
#if defined(MANUAL_ARTIFACT)
#include "Host/ArtifactReductionHostFeature.h"
#endif
#if defined(MANUAL_ROTATION)
#include "Host/ModelRotationHostFeature.h"
#endif
#if defined(MANUAL_ALIGNMENT)
#include "Host/MetrologyAlignmentHostFeature.h"
#endif
namespace Manual {
std::vector<ModulePanel*> BuildModules(TestContext context, QWidget* parent)
{
    std::vector<ModulePanel*> modules;
    auto reference = std::make_shared<ReferenceDataSource>();
    if (!context.runtime.AttachFeature(reference)) throw std::runtime_error("测试输入适配器挂载失败");
    context.workflow.getPublishedGraph = [reference] { return reference->GetPublishedGraph(); };
    modules.push_back(CreateDataTest(context, reference, parent));
    modules.push_back(CreateViewTest(context, parent));
    const auto unavailable = [&](const QString& name) {
        auto* page = new ModulePanel(context, name, parent);
        page->SetNotice("当前构建未启用此功能。"); modules.push_back(page);
    };
    const auto attach = [&](const std::shared_ptr<HostFeature>& feature) {
        if (!context.runtime.AttachFeature(feature)) throw std::runtime_error("功能挂载失败：" + std::string(feature->GetFeatureId()));
    };
#if defined(MANUAL_CROP)
    auto crop = std::make_shared<CropHostFeature>(); attach(crop); modules.push_back(CreateCropTest(context, crop, parent));
#else
    unavailable("Crop");
#endif
#if defined(MANUAL_GAP)
    GapHostConfig gapConfig; gapConfig.defaultStart.targetViews = GetAllViews();
    gapConfig.inputViews = GetAllViews();
    gapConfig.keys.switchOverlay.keySym = "F9";
    gapConfig.keys.exit.keySym = "Escape";
    auto gap = std::make_shared<GapHostFeature>(gapConfig); attach(gap); modules.push_back(CreateGapTest(context, gap, parent));
#else
    unavailable("Gap");
#endif
#if defined(MANUAL_PART)
    PartSegmentationConfig partConfig; partConfig.defaultStart.targetViews = GetPartViews();
    partConfig.maxWorkingBytes = context.workflow.resources.workingBytes;
    partConfig.maxHistoryBytes = context.workflow.resources.publishBytes;
    // 整卷编辑的时限包含表面重建和不可变标签冻结，测试宿主显式给出完整阶段预算。
    partConfig.editTimeoutMs = 120000;
    auto part = std::make_shared<PartSegmentationHostFeature>(partConfig); attach(part);
    modules.push_back(CreatePartTest(context, part, parent)); modules.push_back(CreatePartEditTest(context, part, parent));
#else
    unavailable("Part"); unavailable("PartEdit");
#endif
#if defined(MANUAL_SURFACE)
    SurfaceDeterminationConfig surfaceConfig; surfaceConfig.defaultStart.targetViews = GetMainViews();
    surfaceConfig.maxWorkingBytes = context.workflow.resources.workingBytes;
    auto surface = std::make_shared<SurfaceDeterminationHostFeature>(surfaceConfig); attach(surface);
    modules.push_back(CreateSurfaceTest(context, surface, parent));
#else
    unavailable("Surface");
#endif
#if defined(MANUAL_ARTIFACT)
    ArtifactConfig artifactConfig; artifactConfig.memoryBudgetBytes = context.workflow.resources.workingBytes;
    artifactConfig.publishBudgetBytes = context.workflow.resources.publishBytes;
    auto artifact = std::make_shared<ArtifactReductionHostFeature>(artifactConfig); attach(artifact); modules.push_back(CreateArtifactTest(context, artifact, parent));
#else
    unavailable("Artifact");
#endif
#if defined(MANUAL_ROTATION)
    auto rotation = std::make_shared<ModelRotationHostFeature>(); attach(rotation); modules.push_back(CreateRotationTest(context, rotation, parent));
#else
    unavailable("Rotation");
#endif
#if defined(MANUAL_ALIGNMENT)
    AlignmentConfig alignmentConfig; alignmentConfig.targetViews = GetMainViews();
    auto alignment = std::make_shared<MetrologyAlignmentHostFeature>(alignmentConfig); attach(alignment);
    modules.push_back(CreateAlignmentTest(context, alignment, reference, parent));
#else
    unavailable("Alignment");
#endif
    return modules;
}
}
