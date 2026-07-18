#include "HostCommandRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

class Fixture final {
public:
    Fixture()
        : bindings(std::make_shared<HostFeatureBindings>())
    {
        context = std::make_shared<StdRenderContext>();
        sliceContext = std::make_shared<StdRenderContext>();
        views.CreateView("primary", HostRenderViewRole::Primary3D, context);
        views.CreateView("slice", HostRenderViewRole::TopDownSlice, sliceContext);
        router = std::make_unique<HostCommandRouter>(core, views, bindings);
    }

    bool Send(HostCommand command) const
    {
        return router->DispatchCommand(std::move(command));
    }

    std::shared_ptr<VizService> GetService() const
    {
        const auto* view = views.GetPrimaryView();
        return view ? view->service : nullptr;
    }

    std::shared_ptr<VizService> GetSliceService() const
    {
        const auto* view = views.GetViewBySelector(
            { "slice", false, HostRenderViewRole::TopDownSlice });
        return view ? view->service : nullptr;
    }

    HostCoreServices core;
    HostRenderViewSet views;
    std::shared_ptr<HostFeatureBindings> bindings;
    std::shared_ptr<StdRenderContext> context;
    std::shared_ptr<StdRenderContext> sliceContext;
    std::unique_ptr<HostCommandRouter> router;
};

HostVolumeGeometry BuildGeometry()
{
    return { { 2, 2, 1 }, { 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f } };
}

bool SendData(Fixture& fixture, HostDataAction action, HostDataPayload payload,
    HostCompleteCallback onComplete = nullptr)
{
    return fixture.Send(HostDataCommand{
        HostDataRequest{ action, std::move(payload) }, std::move(onComplete) });
}

void StartDataCases(int& failureCount)
{
    Fixture fixture;
    auto service = fixture.GetService();
    auto sliceService = fixture.GetSliceService();
    bool isCallbackSuccess = false;
    SetExpect(SendData(fixture, HostDataAction::LoadFile,
        HostLoadRequest{ "volume.tiff", BuildGeometry() },
        [&isCallbackSuccess](bool isSuccess) { isCallbackSuccess = isSuccess; }),
        "显式布局加载应被接收。", failureCount);
    SetExpect(service->GetLoadCount() == 1 && isCallbackSuccess,
        "加载应保留布局并完成回调。", failureCount);
    SetExpect(service->GetLoadLayout().GetDimensions() == std::array<int, 3>{ 2, 2, 1 },
        "加载布局维度应完整传递。", failureCount);

    const std::string unicodeLoadPath = u8"C:/体数据 é/输入.tiff";
    SetExpect(
        SendData(fixture, HostDataAction::LoadFile,
            HostLoadRequest{ unicodeLoadPath, BuildGeometry() })
            && service->GetLoadPath() == unicodeLoadPath,
        "Host load 路由必须原样保留 UTF-8 路径字节。",
        failureCount);

    const std::string unicodeExportPath = u8"C:/体数据 é/导出.raw";
    SetExpect(
        SendData(fixture, HostDataAction::ExportVolume,
            HostVolumeExportRequest{ unicodeExportPath })
            && service->GetExportPath() == unicodeExportPath,
        "Host volume export 路由必须原样保留 UTF-8 路径字节。",
        failureCount);

    const std::string unicodeSlicePath = u8"C:/体数据 é/切片";
    HostSliceExportRequest sliceRequest;
    sliceRequest.outputDir = unicodeSlicePath;
    sliceRequest.sourceView.viewId = "slice";
    SetExpect(
        SendData(fixture, HostDataAction::ExportSlices, std::move(sliceRequest))
            && sliceService->GetSlicePath() == unicodeSlicePath,
        "Host slice export 路由必须原样保留 UTF-8 路径字节。",
        failureCount);

    HostLoadRequest rawRequest{ "scan_3x4x5.raw", BuildGeometry() };
    rawRequest.geometry.dimensions = { 0, 0, 0 };
    SetExpect(SendData(fixture, HostDataAction::LoadFile, std::move(rawRequest)),
        "全零 raw 维度应仅从锚定后缀推断。", failureCount);
    SetExpect(service->GetLoadLayout().GetDimensions() == std::array<int, 3>{ 3, 4, 5 },
        "raw 后缀维度应正确解析。", failureCount);

    HostLoadRequest partial{ "scan.raw", BuildGeometry() };
    partial.geometry.dimensions = { 2, 0, 1 };
    SetExpect(!SendData(fixture, HostDataAction::LoadFile, std::move(partial)),
        "部分零维度必须被拒绝。", failureCount);
    SetExpect(!SendData(fixture, HostDataAction::LoadFile,
        HostVolumeExportRequest{ "wrong.raw" }),
        "action 与 payload 不匹配必须被拒绝。", failureCount);

    std::vector<float> voxels{ 1.0f, 2.0f, 3.0f, 4.0f };
    SetExpect(SendData(fixture, HostDataAction::ReloadBuffer,
        HostReloadRequest{ std::move(voxels), BuildGeometry() }),
        "精确大小的 owning reload 应被接收。", failureCount);
    SetExpect(service->GetReloadBuffer().GetVoxels()
            == std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f },
        "reload 必须持有体素快照。", failureCount);
    SetExpect(!SendData(fixture, HostDataAction::ReloadBuffer,
        HostReloadRequest{ std::vector<float>{ 1.0f }, BuildGeometry() }),
        "短 buffer 必须被拒绝。", failureCount);
    SetExpect(!SendData(fixture, HostDataAction::None, std::monostate{}),
        "None data action 必须被拒绝。", failureCount);
}

void StartFeatureCases(int& failureCount)
{
    Fixture fixture;
    HostCropTargetRequest cropTarget;
    cropTarget.referenceView = { "primary", false, HostRenderViewRole::Primary3D };
    HostCropRequest cropStart{ HostCropAction::Start, cropTarget };
    SetExpect(fixture.Send(HostCropCommand{ std::move(cropStart), nullptr }),
        "Crop Start + target payload 应被接收。", failureCount);
    SetExpect(fixture.bindings->GetCropStartCount() == 1,
        "Crop Start 应只投递一次。", failureCount);
    SetExpect(!fixture.Send(HostCropCommand{
        HostCropRequest{ HostCropAction::Preview, cropTarget }, nullptr }),
        "Crop Preview + target payload 必须被拒绝。", failureCount);
    bool isCropDone = false;
    SetExpect(fixture.Send(HostCropCommand{
        HostCropRequest{ HostCropAction::Submit, cropTarget },
        [&isCropDone](bool isSuccess) { isCropDone = isSuccess; } }),
        "Crop Submit 应接收完成回调。", failureCount);
    SetExpect(isCropDone && fixture.bindings->GetCropSendCount() == 1,
        "Crop Submit 回调应精确完成一次。", failureCount);
    SetExpect(fixture.Send(HostCropCommand{
        HostCropRequest{ HostCropAction::Exit, std::monostate{} }, nullptr }),
        "Crop Exit + monostate 应被接收。", failureCount);

    HostGapStartRequest gapStart;
    gapStart.targetViews.viewIds = { "primary" };
    bool isGapDone = false;
    SetExpect(fixture.Send(HostGapCommand{
        HostGapRequest{ HostGapAction::Start, gapStart },
        [&isGapDone](bool isSuccess) { isGapDone = isSuccess; } }),
        "Gap Start + start payload 应被接收。", failureCount);
    SetExpect(isGapDone && fixture.bindings->GetGapStartCount() == 1,
        "Gap Start 回调应精确完成一次。", failureCount);
    SetExpect(fixture.Send(HostGapCommand{
        HostGapRequest{ HostGapAction::Overlay, std::monostate{} }, nullptr }),
        "Gap Overlay + monostate 应被接收。", failureCount);
    SetExpect(!fixture.Send(HostGapCommand{
        HostGapRequest{ HostGapAction::Overlay, gapStart }, nullptr }),
        "Gap Overlay + start payload 必须被拒绝。", failureCount);
    SetExpect(fixture.Send(HostGapCommand{
        HostGapRequest{ HostGapAction::Exit, std::monostate{} }, nullptr }),
        "Gap Exit + monostate 应被接收。", failureCount);
}

} // namespace

int HostRouterSuite::GetFailCount() const
{
    int failureCount = 0;
    StartDataCases(failureCount);
    StartFeatureCases(failureCount);
    return failureCount;
}
