#pragma once
#include <memory>
class GapHostFeature;
class WallThicknessHostFeature;
class SurfaceDeterminationHostFeature;
class MetrologyAlignmentHostFeature;
class ArtifactReductionHostFeature;
namespace Manual {
class TestWorkflow;
#if defined(MANUAL_ARTIFACT)
void AttachReportSource(TestWorkflow&,std::shared_ptr<ArtifactReductionHostFeature>);
#endif
#if defined(MANUAL_GAP)
void AttachReportSource(TestWorkflow&,std::shared_ptr<GapHostFeature>);
#endif
#if defined(MANUAL_WALL)
void AttachReportSource(TestWorkflow&,std::shared_ptr<WallThicknessHostFeature>);
#endif
#if defined(MANUAL_SURFACE)
void AttachReportSource(TestWorkflow&,std::shared_ptr<SurfaceDeterminationHostFeature>);
#endif
#if defined(MANUAL_ALIGNMENT)
void AttachReportSource(TestWorkflow&,std::shared_ptr<MetrologyAlignmentHostFeature>);
#endif
}
