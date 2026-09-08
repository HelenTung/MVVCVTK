// 测试用途：声明功能测试页面的创建入口，供共享测试窗口组合。
#pragma once
#include "ModulePanel.h"
class CropHostFeature;
class GapHostFeature;
class PartSegmentationHostFeature;
class SurfaceDeterminationHostFeature;
class ArtifactReductionHostFeature;
class ModelRotationHostFeature;
class MetrologyAlignmentHostFeature;
namespace Manual {
class ReferenceDataSource;
ModulePanel* CreateDataTest(TestContext, std::shared_ptr<ReferenceDataSource>, QWidget*);
ModulePanel* CreateViewTest(TestContext, QWidget*);
ModulePanel* CreateCropTest(TestContext, std::shared_ptr<CropHostFeature>, QWidget*);
ModulePanel* CreateGapTest(TestContext, std::shared_ptr<GapHostFeature>, QWidget*);
ModulePanel* CreatePartTest(TestContext, std::shared_ptr<PartSegmentationHostFeature>, QWidget*);
ModulePanel* CreatePartEditTest(TestContext, std::shared_ptr<PartSegmentationHostFeature>, QWidget*);
ModulePanel* CreateSurfaceTest(TestContext, std::shared_ptr<SurfaceDeterminationHostFeature>, QWidget*);
ModulePanel* CreateArtifactTest(TestContext, std::shared_ptr<ArtifactReductionHostFeature>, QWidget*);
ModulePanel* CreateRotationTest(TestContext, std::shared_ptr<ModelRotationHostFeature>, QWidget*);
ModulePanel* CreateAlignmentTest(TestContext, std::shared_ptr<MetrologyAlignmentHostFeature>,
    std::shared_ptr<ReferenceDataSource>, QWidget*);
}
