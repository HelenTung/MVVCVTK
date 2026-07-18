#include "QtHostMethodCases.h"

#include "AppState.h"
#include "AppService.h"
#include "DataManager.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/CropBridge.h"
#include "Services/GapAnalysisService.h"
#include "Host/VtkAppHostSession.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include <memory>
#include <vector>

namespace {

vtkSmartPointer<vtkImageData> GetCropImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(3, 4, 2);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* scalars = static_cast<float*>(image->GetScalarPointer());
    for (vtkIdType id = 0; id < image->GetNumberOfPoints(); ++id) {
        scalars[id] = static_cast<float>(id + 1);
    }
    image->Modified();
    return image;
}

}

int GetCropFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    const HostCropTargetRequest target;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Start, target }),
        "Crop start missing reference rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Preview, target }),
        "Crop preview payload mismatch rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendCrop({ HostCropAction::Exit, std::monostate{} }),
        "Crop inactive exit rejection") ? 0 : 1;

    for (const std::size_t failedViewIndex : { std::size_t{0}, std::size_t{1} }) {
        HostCoreServices core;
        core.sharedDataMgr = std::make_shared<RawVolumeDataManager>();
        core.sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
        core.sharedState = std::make_shared<SharedInteractionState>(
            core.sharedStateBroadcaster);
        core.gapAnalysis = std::make_shared<GapAnalysisService>();
        core.orthogonalCropBridge = std::make_shared<CropBridge>();

        std::vector<HostRenderViewConfig> configs(2);
        configs[0].id = "crop-primary";
        configs[0].role = HostRenderViewRole::Primary3D;
        configs[1].id = "crop-secondary";
        configs[1].role = HostRenderViewRole::Primary3D;

        HostRenderViewSet views;
        if (!GetCaseResult(
            views.Build(core, configs),
            "Crop transaction fixture builds real render views")) {
            ++failureCount;
            continue;
        }
        HostFeatureBindings bindings;
        bindings.AttachFeatures(core, views);
        views.SetInteractorsReady();

        auto oldImage = GetCropImage();
        double oldRange[2] = { 0.0, 0.0 };
        oldImage->GetScalarRange(oldRange);
        bool hasPending = false;
        const bool isCurrentReady = core.sharedState->StartLoad(LoadEventKind::File)
            && core.sharedDataMgr->SetImageSnapshot(oldImage)
            && core.sharedDataMgr->SetCurrentFromPending(hasPending)
            && hasPending
            && core.sharedState->SetFileDataReady(
                oldRange[0], oldRange[1], { 1.0, 1.0, 1.0 })
            && core.sharedState->ResetLoad(LoadEventKind::File);
        if (!GetCaseResult(
            isCurrentReady,
            "Crop transaction fixture establishes trusted old current")) {
            ++failureCount;
            continue;
        }

        HostCropTargetRequest request;
        request.referenceView.viewId = "crop-primary";
        const auto before = core.sharedDataMgr->GetImageState();
        const DataVersion beforeVersion = before.version;
        bool hasCallback = false;

        if (!GetCaseResult(
            bindings.SwitchCropBox(request),
            "Crop transaction fixture enters box editing before submit")) {
            ++failureCount;
            continue;
        }
        views.GetViews()[failedViewIndex].service->SetRenderContext(nullptr, nullptr);
        const bool isAccepted = bindings.SendCrop(
            request,
            [&hasCallback](bool) { hasCallback = true; });

        const auto after = core.sharedDataMgr->GetImageState();
        failureCount += GetCaseResult(
            !isAccepted,
            "Pipeline failure rejects crop submit") ? 0 : 1;
        failureCount += GetCaseResult(
            !hasCallback,
            "Rejected submit does not publish success callback") ? 0 : 1;
        failureCount += GetCaseResult(
            bindings.GetCropActive(),
            "Failed crop submit remains editable") ? 0 : 1;
        failureCount += GetCaseResult(
            after.dims == before.dims
                && after.spacing == before.spacing
                && after.origin == before.origin
                && after.scalarRange == before.scalarRange,
            "Pipeline failure restores complete previous current batch") ? 0 : 1;
        failureCount += GetCaseResult(
            after.version == beforeVersion + 2,
            "Promote and compensate preserve monotonic DataVersion") ? 0 : 1;
        failureCount += GetCaseResult(
            core.sharedState->GetReloadLoadState() == LoadState::Failed,
            "Pipeline compensation publishes ReloadFailed after restore") ? 0 : 1;
    }
    return failureCount;
}
