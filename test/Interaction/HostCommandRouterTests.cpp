#include "HostCommandRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostRoutes.h"
#include "Host/Types/HostRequestTypes.h"
#include "ViewContext.h"

#include <array>
#include <cmath>
#include <limits>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(static_cast<int>(HostVolumeQuality::Auto) == 0);
static_assert(static_cast<int>(HostVolumeQuality::Low) == 1);
static_assert(static_cast<int>(HostVolumeQuality::High) == 2);
static_assert(static_cast<int>(HostVolumeQuality::XHigh) == 3);
static_assert(static_cast<int>(HostVolumeQuality::Ultra) == 4);
static_assert(
    static_cast<int>(HostDataExportFormat::Raw) == 0);
static_assert(std::is_polymorphic_v<HostRequest>);
static_assert(std::has_virtual_destructor_v<HostRequest>);
static_assert(std::is_base_of_v<HostRequest, HostLoadRequest>);
static_assert(std::is_base_of_v<HostRequest, HostReloadRequest>);
static_assert(std::is_base_of_v<HostRequest, HostDataExportRequest>);
static_assert(std::is_base_of_v<HostRequest, HostSliceExportRequest>);
static_assert(std::is_base_of_v<HostRequest, HostViewSetRequest>);
static_assert(std::is_base_of_v<HostRequest, HostSessionSetRequest>);
static_assert(std::is_base_of_v<HostRequest, HostViewResetRequest>);
static_assert(std::is_base_of_v<HostRequest, HostToolSetRequest>);
static_assert(std::is_base_of_v<HostRequest, HostToolSwitchRequest>);
static_assert(std::is_final_v<HostLoadRequest>);
static_assert(std::is_final_v<HostReloadRequest>);
static_assert(std::is_final_v<HostDataExportRequest>);
static_assert(std::is_final_v<HostSliceExportRequest>);
static_assert(std::is_final_v<HostViewSetRequest>);
static_assert(std::is_final_v<HostSessionSetRequest>);
static_assert(std::is_final_v<HostViewResetRequest>);
static_assert(std::is_final_v<HostToolSetRequest>);
static_assert(std::is_final_v<HostToolSwitchRequest>);

struct UnknownHostRequest final : HostRequest {
};

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
    {
        context = std::make_shared<ViewContextStub>();
        sliceContext = std::make_shared<ViewContextStub>();
        views->CreateView("primary", HostRenderViewRole::Primary3D, context);
        views->CreateView("slice", HostRenderViewRole::TopDownSlice, sliceContext);
        router = std::make_unique<HostCommandRouter>(
            views->GetViewDirectory());
    }

    bool Send(
        HostRequest&& request,
        HostCompleteCallback onComplete = nullptr) const
    {
        return router->Dispatch(
            std::move(request),
            std::move(onComplete));
    }

    std::shared_ptr<AppPortState> GetService() const
    {
        return views->GetState("primary");
    }

    std::shared_ptr<AppPortState> GetSliceService() const
    {
        return views->GetState("slice");
    }

    std::shared_ptr<HostRouteStub> views =
        std::make_shared<HostRouteStub>();
    std::shared_ptr<ViewContextStub> context;
    std::shared_ptr<ViewContextStub> sliceContext;
    std::unique_ptr<HostCommandRouter> router;
};

HostVolumeGeometry BuildGeometry()
{
    HostVolumeGeometry geometry;
    geometry.dimensions = { 2, 2, 1 };
    geometry.spacing = { 1.0f, 2.0f, 3.0f };
    geometry.origin = { 4.0f, 5.0f, 6.0f };
    geometry.direction = {
        0.0, -1.0, 0.0,
        1.0, 0.0, 0.0,
        0.0, 0.0, 1.0
    };
    return geometry;
}

ImageMetadata BuildMetadata(const ImageSourceKind sourceKind)
{
    ImageMetadata metadata;
    metadata.identity.datasetId = "dataset-001";
    metadata.identity.inspectionId = "inspection-001";
    metadata.source.kind = sourceKind;
    metadata.source.uri = sourceKind == ImageSourceKind::Memory
        ? "memory://dataset-001" : "file://dataset-001";
    metadata.scalar.quantity = "gray";
    metadata.scalar.unit = "1";
    metadata.attributes.push_back({ "modality", std::string{ "CT" } });
    return metadata;
}

HostVolumeTransferFunction GetVolumeTransferFunction();

template <typename Request>
bool SendData(
    Fixture& fixture,
    Request request,
    HostCompleteCallback onComplete = nullptr)
{
    static_assert(std::is_base_of_v<HostRequest, Request>);
    return fixture.Send(
        std::move(request),
        std::move(onComplete));
}

bool SendView(Fixture& fixture, HostViewSetRequest request)
{
    return fixture.Send(std::move(request));
}

bool SendSession(Fixture& fixture, HostSessionSetRequest request)
{
    return fixture.Send(std::move(request));
}

HostLoadRequest GetLoadRequest(
    std::string filePath,
    HostVolumeGeometry geometry)
{
    HostLoadRequest request;
    request.filePath = std::move(filePath);
    request.geometry = std::move(geometry);
    request.metadata = BuildMetadata(ImageSourceKind::TiffSeries);
    return request;
}

HostReloadRequest GetReloadReq(
    std::vector<float> voxels,
    HostVolumeGeometry geometry)
{
    HostReloadRequest request;
    request.voxels = std::move(voxels);
    request.geometry = std::move(geometry);
    request.metadata = BuildMetadata(ImageSourceKind::Memory);
    return request;
}

HostViewSetRequest BuildViewRequest()
{
    HostViewSetRequest request;
    request.targetView.viewId = "primary";
    request.mode = HostRenderMode::IsoSurface;
    request.material = HostMaterialParams{};
    request.opacity = 0.8;
    request.volumeTransferFunction = GetVolumeTransferFunction();
    request.iso = 0.5;
    request.background = HostBackgroundColor{};
    request.windowLevel = HostWindowLevelParams{};
    request.visibility = HostVisibilityParams{
        true, false, true
    };
    request.isAxesVisible = true;
    return request;
}

void StartViewCases(int& failureCount)
{
    const auto getIsRejected = [&failureCount](
        HostViewSetRequest request, const char* message) {
        Fixture fixture;
        const auto service = fixture.GetService();
        SetExpect(!SendView(fixture, std::move(request)), message, failureCount);
        SetExpect(service->GetViewSetCount() == 0
                && fixture.context->GetCameraStyleSetCount() == 0,
            "非法 View 请求不得调用任何 setter。", failureCount);
    };

    auto request = BuildViewRequest();

    Fixture invalidSession;
    HostSessionSetRequest sessionRequest;
    sessionRequest.spacing =
        std::array<double, 3>{ 1.0, 0.0, 3.0 };
    SetExpect(!SendSession(invalidSession, std::move(sessionRequest))
            && invalidSession.GetService()->GetSpacingSetCount() == 0,
        "非法 Session spacing 必须整笔拒绝。", failureCount);

    request = BuildViewRequest();
    request.opacity = std::numeric_limits<double>::quiet_NaN();
    getIsRejected(std::move(request), "NaN opacity 必须整笔拒绝。");

    request = BuildViewRequest();
    request.iso = std::numeric_limits<double>::infinity();
    getIsRejected(std::move(request), "Inf iso 必须整笔拒绝。");

    request = BuildViewRequest();
    request.material->ambient = 1.1;
    getIsRejected(std::move(request), "越界 material 必须整笔拒绝。");

    request = BuildViewRequest();
    request.background->b = -0.1;
    getIsRejected(std::move(request), "越界 background 必须整笔拒绝。");

    request = BuildViewRequest();
    request.volumeTransferFunction = HostVolumeTransferFunction{};
    getIsRejected(std::move(request), "显式空 TF 必须整笔拒绝。");

    request = BuildViewRequest();
    request.volumeTransferFunction->colorNodes.at(0).r = 1.1;
    getIsRejected(std::move(request), "越界 TF node 必须整笔拒绝。");

    request = BuildViewRequest();
    request.volumeTransferFunction->colorNodes.at(0).scalar = 1000.0;
    request.volumeTransferFunction->colorNodes.at(1).scalar = 500.0;
    getIsRejected(std::move(request), "下降 TF scalar 必须整笔拒绝。");

    request = BuildViewRequest();
    request.volumeTransferFunction->colorNodes.at(1).scalar =
        request.volumeTransferFunction->colorNodes.at(0).scalar;
    getIsRejected(std::move(request), "重复 TF color scalar 必须整笔拒绝。");

    request = BuildViewRequest();
    request.volumeTransferFunction->opacityNodes.at(1).scalar =
        request.volumeTransferFunction->opacityNodes.at(0).scalar;
    getIsRejected(std::move(request), "重复 TF opacity scalar 必须整笔拒绝。");

    request = BuildViewRequest();
    request.windowLevel->windowWidth = 0.0;
    getIsRejected(std::move(request), "非法 window/level 必须整笔拒绝。");

    sessionRequest = HostSessionSetRequest{};
    sessionRequest.cursor = HostCursorParams{};
    sessionRequest.cursor->world[1] =
        std::numeric_limits<double>::quiet_NaN();
    SetExpect(!SendSession(invalidSession, std::move(sessionRequest)),
        "非有限 Session cursor 必须整笔拒绝。", failureCount);

    sessionRequest = HostSessionSetRequest{};
    sessionRequest.cursor = HostCursorParams{};
    sessionRequest.cursor->axis = 3;
    SetExpect(!SendSession(invalidSession, std::move(sessionRequest)),
        "越界 Session cursor axis 必须整笔拒绝。", failureCount);

    request = BuildViewRequest();
    request.materialPreset = HostMaterialPreset::Glossy;
    getIsRejected(
        std::move(request),
        "材质 preset 与数值 material/opacity 冲突时必须整笔拒绝。");

    request = HostViewSetRequest{};
    request.targetView.viewId = "primary";
    request.mode = HostRenderMode::Volume;
    request.volumeQuality = static_cast<HostVolumeQuality>(99);
    getIsRejected(std::move(request), "非法 quality 枚举必须整笔拒绝。");

    request = HostViewSetRequest{};
    request.targetView.viewId = "slice";
    request.mode = HostRenderMode::SliceTopDown;
    request.volumeQuality = HostVolumeQuality::High;
    getIsRejected(
        std::move(request),
        "Slice 目标不得接收 Volume quality。");

    Fixture fixture;
    SetExpect(SendView(fixture, BuildViewRequest()),
        "全量合法 View 请求应被接收。", failureCount);
    SetExpect(fixture.GetService()->GetViewSetCount() == 9
            && fixture.context->GetCameraStyleSetCount() == 1
            && fixture.context->GetAxesSetCount() == 1,
        "合法 View 请求应按完整字段一次提交。", failureCount);
    SetExpect(fixture.GetService()->GetMaterialSetCount() == 1
            && fixture.GetService()->GetOpacitySetCount() == 0,
        "material 与独立 opacity 必须合并成一次 Material 提交。",
        failureCount);
    HostSessionSetRequest linkedRequest;
    linkedRequest.spacing =
        std::array<double, 3>{ 1.0, 2.0, 3.0 };
    linkedRequest.cursor = HostCursorParams{
        { 4.0, 5.0, 6.0 }, 2
    };
    SetExpect(SendSession(fixture, std::move(linkedRequest)),
        "合法 Session 联动请求应被接收。", failureCount);
    SetExpect(fixture.GetService()->GetCursorSetCount() == 1
            && fixture.GetService()->GetCursorWorld()
                == std::array<double, 3>{ 4.0, 5.0, 6.0 }
            && fixture.GetService()->GetCursorAxis() == 2
            && fixture.GetService()->GetVisibilitySetCount() == 3
            && fixture.GetService()->GetVisibilityMask()
                == (VisFlags::Planes3D | VisFlags::Ruler)
            && fixture.context->GetAxesVisible()
            && fixture.GetService()->GetDirtySetCount() == 1,
        "Session cursor 与 View 显隐/方向轴必须进入各自端口。",
        failureCount);

    const int viewSetCount =
        fixture.GetService()->GetViewSetCount();
    const int materialSetCount =
        fixture.GetService()->GetMaterialSetCount();
    const int opacitySetCount =
        fixture.GetService()->GetOpacitySetCount();
    const int cameraStyleCount =
        fixture.context->GetCameraStyleSetCount();
    HostViewSetRequest isoPatch;
    isoPatch.targetView.viewId = "primary";
    isoPatch.iso = 0.42;
    SetExpect(
        SendView(fixture, std::move(isoPatch))
            && fixture.GetService()->GetViewSetCount()
                == viewSetCount + 1
            && fixture.GetService()->GetMaterialSetCount()
                == materialSetCount
            && fixture.GetService()->GetOpacitySetCount()
                == opacitySetCount
            && fixture.context->GetCameraStyleSetCount()
                == cameraStyleCount,
        "仅设置 ISO 的 patch 不得改写其它 View 状态。",
        failureCount);

    Fixture spacingFixture;
    spacingFixture.GetService()->SetSpacingAccepted(false);
    HostSessionSetRequest spacingRequest;
    spacingRequest.spacing =
        std::array<double, 3>{ 1.0, 2.0, 3.0 };
    SetExpect(!SendSession(spacingFixture, std::move(spacingRequest)),
        "spacing 运行时拒绝必须向 Router 返回失败。", failureCount);
    SetExpect(spacingFixture.GetService()->GetSpacingSetCount() == 1
            && spacingFixture.GetService()->GetViewSetCount() == 0
            && spacingFixture.context->GetCameraStyleSetCount() == 0,
        "spacing 失败前不得提交其它 View 状态。", failureCount);

    Fixture qualityFixture;
    qualityFixture.GetService()->SetQualityAccepted(false);
    HostViewSetRequest qualityRequest;
    qualityRequest.targetView.viewId = "primary";
    qualityRequest.mode = HostRenderMode::Volume;
    qualityRequest.material = HostMaterialParams{};
    qualityRequest.volumeQuality = HostVolumeQuality::Auto;
    qualityRequest.isAxesVisible = true;
    SetExpect(!SendView(qualityFixture, std::move(qualityRequest)),
        "quality 运行时拒绝必须向 Router 返回失败。", failureCount);
    SetExpect(qualityFixture.GetService()->GetQualitySetCount() == 1
            && qualityFixture.GetService()->GetViewSetCount() == 0
            && qualityFixture.GetService()->GetMaterialSetCount() == 0
            && qualityFixture.context->GetCameraStyleSetCount() == 0
            && qualityFixture.context->GetAxesSetCount() == 0,
        "quality 失败必须发生在所有 void View setter 之前。",
        failureCount);

    Fixture cameraFail;
    cameraFail.context->SetCameraFailCount(1);
    HostViewSetRequest cameraRequest;
    cameraRequest.targetView.viewId = "primary";
    cameraRequest.mode = HostRenderMode::IsoSurface;
    cameraRequest.iso = 0.35;
    SetExpect(!SendView(cameraFail, std::move(cameraRequest))
            && cameraFail.GetService()->GetVizMode() == VizMode::Volume
            && cameraFail.GetService()->GetIsoThreshold() == 0.0
            && cameraFail.GetService()->GetRevision() == 2
            && cameraFail.context->GetVizMode() == VizMode::Volume
            && cameraFail.context->GetCameraStyleSetCount() == 2
            && cameraFail.GetService()->GetIsAvailable(),
        "camera 提交失败必须按 revision 恢复 App 与 context。",
        failureCount);

    Fixture axesFail;
    axesFail.context->SetAxesFailCount(1);
    HostViewSetRequest axesRequest;
    axesRequest.targetView.viewId = "primary";
    axesRequest.iso = 0.45;
    axesRequest.isAxesVisible = true;
    SetExpect(!SendView(axesFail, std::move(axesRequest))
            && axesFail.GetService()->GetIsoThreshold() == 0.0
            && axesFail.GetService()->GetRevision() == 2
            && !axesFail.context->GetAxesVisible()
            && axesFail.context->GetAxesSetCount() == 2
            && axesFail.GetService()->GetDirtySetCount() == 0
            && axesFail.GetService()->GetIsAvailable(),
        "axes 提交失败必须恢复 App/axes 且不得标脏。",
        failureCount);

    Fixture dirtyFail;
    dirtyFail.GetService()->SetDirtyAccepted(false);
    HostViewSetRequest dirtyRequest;
    dirtyRequest.targetView.viewId = "primary";
    dirtyRequest.iso = 0.55;
    dirtyRequest.isAxesVisible = true;
    SetExpect(!SendView(dirtyFail, std::move(dirtyRequest))
            && dirtyFail.GetService()->GetIsoThreshold() == 0.0
            && dirtyFail.GetService()->GetRevision() == 2
            && !dirtyFail.context->GetAxesVisible()
            && dirtyFail.context->GetAxesSetCount() == 2
            && dirtyFail.GetService()->GetDirtySetCount() == 0,
        "dirty 拒绝必须逆序恢复 axes 与 App。",
        failureCount);

    Fixture restoreFail;
    restoreFail.context->SetCameraFailCount(1);
    restoreFail.GetService()->SetRestoreAccepted(false);
    HostViewSetRequest restoreRequest;
    restoreRequest.targetView.viewId = "primary";
    restoreRequest.mode = HostRenderMode::IsoSurface;
    restoreRequest.iso = 0.65;
    SetExpect(!SendView(restoreFail, std::move(restoreRequest))
            && !restoreFail.GetService()->GetIsAvailable(),
        "补偿失败必须停用目标 view。", failureCount);
    const auto restoreRevision =
        restoreFail.GetService()->GetRevision();
    HostViewSetRequest stoppedRequest;
    stoppedRequest.targetView.viewId = "primary";
    stoppedRequest.iso = 0.75;
    SetExpect(!SendView(restoreFail, std::move(stoppedRequest))
            && restoreFail.GetService()->GetRevision()
                == restoreRevision,
        "已停用 view 不得继续接收跨层请求。", failureCount);

    Fixture extendedFixture;
    HostViewSetRequest extended;
    extended.targetView.viewId = "primary";
    extended.mode = HostRenderMode::Volume;
    extended.materialPreset = HostMaterialPreset::Glossy;
    extended.volumeQuality = HostVolumeQuality::XHigh;
    extended.volumeTransferFunction = GetVolumeTransferFunction();
    SetExpect(SendView(extendedFixture, std::move(extended)),
        "合法扩展显示契约应一次提交。", failureCount);
    const auto extendedService = extendedFixture.GetService();
    SetExpect(extendedService->GetMaterialSetCount() == 1
            && extendedService->GetMaterial().isShadeOn
            && extendedService->GetQualitySetCount() == 1
            && extendedService->GetVolumeQuality()
                == VolumeQuality::XHigh
            && extendedService->GetVolumeTransferFunction()
                .colorNodes.size() == 3
            && extendedService->GetVolumeTransferFunction()
                .opacityNodes.size() == 3
            && extendedFixture.GetSliceService()->GetViewSetCount() == 0,
        "材质、质量和完整 TF 必须映射到单一目标 service。",
        failureCount);

    constexpr std::array qualityCases{
        std::pair{ HostVolumeQuality::Auto, VolumeQuality::Auto },
        std::pair{ HostVolumeQuality::Low, VolumeQuality::Low },
        std::pair{ HostVolumeQuality::High, VolumeQuality::High },
        std::pair{ HostVolumeQuality::XHigh, VolumeQuality::XHigh },
        std::pair{ HostVolumeQuality::Ultra, VolumeQuality::Ultra }
    };
    for (const auto& [hostQuality, appQuality] : qualityCases) {
        Fixture qualityMapFixture;
        HostViewSetRequest qualityMap;
        qualityMap.targetView.viewId = "primary";
        qualityMap.mode = HostRenderMode::Volume;
        qualityMap.volumeQuality = hostQuality;
        SetExpect(
            SendView(qualityMapFixture, std::move(qualityMap))
                && qualityMapFixture.GetService()->GetVolumeQuality()
                    == appQuality,
            "五档 Host quality 必须无损映射到 App 枚举。",
            failureCount);
    }

    HostViewResetRequest reset;
    reset.targetView.viewId = "primary";
    SetExpect(fixture.Send(std::move(reset))
            && fixture.context->GetCameraResetCount() == 1
            && fixture.GetService()->GetDirtySetCount() == 2,
        "ResetCamera 必须只作用于请求目标 context。",
        failureCount);
    reset = HostViewResetRequest{};
    reset.targetView.viewId = "missing";
    SetExpect(!fixture.Send(std::move(reset))
            && fixture.context->GetCameraResetCount() == 1
            && fixture.GetService()->GetDirtySetCount() == 2,
        "ResetCamera 目标未命中时不得复位或标脏。",
        failureCount);

    Fixture resetDirtyFail;
    resetDirtyFail.GetService()->SetDirtyAccepted(false);
    HostViewResetRequest resetFailRequest;
    resetFailRequest.targetView.viewId = "primary";
    SetExpect(!resetDirtyFail.Send(std::move(resetFailRequest))
            && resetDirtyFail.context->GetCameraResetCount() == 1
            && resetDirtyFail.context->GetCameraRestoreCount() == 1
            && resetDirtyFail.GetService()->GetDirtySetCount() == 0
            && resetDirtyFail.GetService()->GetIsAvailable(),
        "ResetCamera 的 dirty 失败必须恢复完整相机快照。",
        failureCount);

    HostViewSetRequest callbackView;
    callbackView.targetView.viewId = "primary";
    callbackView.opacity = 0.65;
    int callbackCount = 0;
    bool isCallbackSuccess = false;
    SetExpect(fixture.Send(
            std::move(callbackView),
            [&callbackCount, &isCallbackSuccess](const bool isSuccess) {
                ++callbackCount;
                isCallbackSuccess = isSuccess;
            })
            && callbackCount == 1
            && isCallbackSuccess,
        "同步 View 请求必须执行并同步回调一次。",
        failureCount);
}

void StartDataCases(int& failureCount)
{
    Fixture fixture;
    auto service = fixture.GetService();
    auto sliceService = fixture.GetSliceService();
    bool isCallbackSuccess = false;
    SetExpect(SendData(
        fixture,
        GetLoadRequest("volume.tiff", BuildGeometry()),
        [&isCallbackSuccess](bool isSuccess) { isCallbackSuccess = isSuccess; }),
        "显式布局加载应被接收。", failureCount);
    SetExpect(service->GetLoadCount() == 1 && isCallbackSuccess,
        "加载应保留布局并完成回调。", failureCount);
    SetExpect(service->GetLoadLayout().GetDimensions() == std::array<int, 3>{ 2, 2, 1 },
        "加载布局维度应完整传递。", failureCount);
    SetExpect(
        service->GetLoadLayout().GetDirection() == BuildGeometry().direction
            && service->GetLoadLayout().GetMetadata().identity.datasetId
                == "dataset-001"
            && service->GetLoadLayout().GetMetadata().source.kind
                == ImageSourceKind::TiffSeries,
        "加载必须把一般 LPS direction 与 descriptor metadata 放进同一布局快照。",
        failureCount);

    const std::string unicodeLoadPath = u8"C:/体数据 é/输入.tiff";
    SetExpect(
        SendData(
            fixture,
            GetLoadRequest(unicodeLoadPath, BuildGeometry()))
            && service->GetLoadPath() == unicodeLoadPath,
        "Host load 路由必须原样保留 UTF-8 路径字节。",
        failureCount);

    const std::array exportCases = {
        std::pair{ HostDataExportFormat::Raw, std::string{ ".raw" } },
        std::pair{ HostDataExportFormat::Ply, std::string{ ".ply" } },
        std::pair{ HostDataExportFormat::Stl, std::string{ ".stl" } },
        std::pair{ HostDataExportFormat::Obj, std::string{ ".obj" } }
    };
    for (const auto& [format, suffix] : exportCases) {
        HostDataExportRequest exportRequest;
        exportRequest.outputPath =
            u8"C:/体数据 é/显式导出";
        exportRequest.format = format;
        const std::string expectedDir =
            exportRequest.outputPath;
        SetExpect(
            SendData(fixture, std::move(exportRequest))
                && service->GetExportDir()
                    == expectedDir
                && service->GetExportExtension()
                    == suffix,
            "显式导出必须把 UTF-8 目录与规范格式后缀沿统一链传递。",
            failureCount);
    }

    HostDataExportRequest inferredRaw;
    inferredRaw.outputPath =
        u8"C:/体数据 é/窗口推断";
    SetExpect(
        SendData(fixture, std::move(inferredRaw))
            && service->GetExportDir()
                == u8"C:/体数据 é/窗口推断"
            && service->GetExportExtension()
                == ".raw",
        "Volume 窗口必须把缺省格式收敛为 RAW。",
        failureCount);

    service->SetVizMode(VizMode::IsoSurface);
    HostDataExportRequest inferredMesh;
    inferredMesh.outputPath =
        u8"C:/体数据 é/窗口推断";
    SetExpect(
        SendData(fixture, std::move(inferredMesh))
            && service->GetExportDir()
                == u8"C:/体数据 é/窗口推断"
            && service->GetExportExtension()
                == ".ply",
        "IsoSurface 窗口必须把缺省格式收敛为 PLY。",
        failureCount);

    service->SetVizMode(VizMode::SliceTop_down);
    HostDataExportRequest inferredSlice;
    inferredSlice.outputPath = "exports";
    SetExpect(
        !SendData(fixture, std::move(inferredSlice)),
        "切片窗口不能推断体数据或网格格式。",
        failureCount);

    HostDataExportRequest invalidFormat;
    invalidFormat.outputPath = "exports";
    invalidFormat.format =
        static_cast<HostDataExportFormat>(99);
    SetExpect(
        !SendData(fixture, std::move(invalidFormat)),
        "显式未知格式必须在 Host 边界被拒绝。",
        failureCount);

    HostDataExportRequest emptyOutputDir;
    emptyOutputDir.format = HostDataExportFormat::Raw;
    SetExpect(
        !SendData(fixture, std::move(emptyOutputDir)),
        "空输出目录必须在 Host 边界被拒绝。",
        failureCount);

    const std::string unicodeSlicePath = u8"C:/体数据 é/切片";
    HostSliceExportRequest sliceRequest;
    sliceRequest.outputDir = unicodeSlicePath;
    sliceRequest.sourceView.viewId = "slice";
    SetExpect(
        SendData(fixture, std::move(sliceRequest))
            && sliceService->GetSlicePath() == unicodeSlicePath,
        "Host slice export 路由必须原样保留 UTF-8 路径字节。",
        failureCount);

    HostLoadRequest rawRequest =
        GetLoadRequest("scan_3x4x5.raw", BuildGeometry());
    rawRequest.metadata.source.kind = ImageSourceKind::RawFile;
    rawRequest.geometry.dimensions = { 0, 0, 0 };
    SetExpect(SendData(fixture, std::move(rawRequest)),
        "全零 raw 维度应仅从锚定后缀推断。", failureCount);
    SetExpect(service->GetLoadLayout().GetDimensions() == std::array<int, 3>{ 3, 4, 5 },
        "raw 后缀维度应正确解析。", failureCount);

    HostLoadRequest tiffWithoutDimensions =
        GetLoadRequest("scan_3x4x5.raw", BuildGeometry());
    tiffWithoutDimensions.geometry.dimensions = { 0, 0, 0 };
    SetExpect(!SendData(fixture, std::move(tiffWithoutDimensions)),
        "TIFF 来源不得借 RAW 文件名后缀推断维度。", failureCount);

    HostLoadRequest partial =
        GetLoadRequest("scan.raw", BuildGeometry());
    partial.metadata.source.kind = ImageSourceKind::RawFile;
    partial.geometry.dimensions = { 2, 0, 1 };
    SetExpect(!SendData(fixture, std::move(partial)),
        "部分零维度必须被拒绝。", failureCount);

    HostLoadRequest missingIdentity =
        GetLoadRequest("scan.raw", BuildGeometry());
    missingIdentity.metadata.source.kind = ImageSourceKind::RawFile;
    missingIdentity.metadata.identity.datasetId.clear();
    SetExpect(!SendData(fixture, std::move(missingIdentity)),
        "缺失 datasetId 的加载必须在 Host admission 边界被拒绝。",
        failureCount);

    HostLoadRequest wrongFileSource =
        GetLoadRequest("scan.raw", BuildGeometry());
    wrongFileSource.metadata.source.kind = ImageSourceKind::Memory;
    SetExpect(!SendData(fixture, std::move(wrongFileSource)),
        "文件加载不得接收 Memory source kind。", failureCount);

    HostReloadRequest wrongMemorySource =
        GetReloadReq(std::vector<float>(4, 1.0f), BuildGeometry());
    wrongMemorySource.metadata.source.kind = ImageSourceKind::RawFile;
    SetExpect(!SendData(fixture, std::move(wrongMemorySource)),
        "内存 reload 必须显式声明 Memory source kind。", failureCount);

    HostReloadRequest singularDirection =
        GetReloadReq(std::vector<float>(4, 1.0f), BuildGeometry());
    singularDirection.geometry.direction.fill(0.0);
    SetExpect(!SendData(fixture, std::move(singularDirection)),
        "奇异 direction 必须在分配与异步 admission 前被拒绝。",
        failureCount);

    HostReloadRequest duplicateAttribute =
        GetReloadReq(std::vector<float>(4, 1.0f), BuildGeometry());
    duplicateAttribute.metadata.attributes.push_back(
        { "modality", std::int64_t{ 7 } });
    SetExpect(!SendData(fixture, std::move(duplicateAttribute)),
        "重复 metadata attribute key 必须整笔拒绝。", failureCount);

    std::vector<float> voxels{ 1.0f, 2.0f, 3.0f, 4.0f };
    SetExpect(SendData(
        fixture,
        GetReloadReq(std::move(voxels), BuildGeometry())),
        "精确大小的 owning reload 应被接收。", failureCount);
    SetExpect(service->GetReloadBuffer().GetVoxels()
            == std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f },
        "reload 必须持有体素快照。", failureCount);
    SetExpect(
        service->GetReloadBuffer().GetLayout().GetMetadata().source.kind
                == ImageSourceKind::Memory
            && service->GetReloadBuffer().GetLayout().GetMetadata()
                .identity.datasetId == "dataset-001",
        "reload 必须把 Memory descriptor metadata 与 voxel owner 一起传递。",
        failureCount);
    SetExpect(!SendData(
        fixture,
        GetReloadReq(std::vector<float>{ 1.0f }, BuildGeometry())),
        "短 buffer 必须被拒绝。", failureCount);

    HostRequest emptyRequest;
    SetExpect(!fixture.Send(std::move(emptyRequest)),
        "空 HostRequest 基类必须被拒绝。", failureCount);
    UnknownHostRequest unknown;
    SetExpect(!fixture.Send(std::move(unknown)),
        "未知 HostRequest 派生类型必须被拒绝。", failureCount);
}

void StartToolCases(int& failureCount)
{
    Fixture fixture;
    HostToolSetRequest setRequest;
    setRequest.targetView.viewId = "primary";
    setRequest.toolMode = HostToolMode::ModelTransform;
    SetExpect(
        fixture.Send(std::move(setRequest))
            && fixture.context->GetToolMode()
                == ToolMode::ModelTransform,
        "Tool Set 必须写入显式模式。",
        failureCount);

    HostToolSwitchRequest switchRequest;
    switchRequest.targetView.viewId = "primary";
    SetExpect(
        fixture.Send(std::move(switchRequest))
            && fixture.context->GetToolMode()
                == ToolMode::Navigation,
        "Tool Switch 必须在预设模式间切换。",
        failureCount);

    HostToolSetRequest callbackTool;
    callbackTool.targetView.viewId = "primary";
    int callbackCount = 0;
    bool isCallbackSuccess = false;
    SetExpect(
        fixture.Send(
            std::move(callbackTool),
            [&callbackCount, &isCallbackSuccess](const bool isSuccess) {
                ++callbackCount;
                isCallbackSuccess = isSuccess;
            })
            && callbackCount == 1
            && isCallbackSuccess,
        "同步 Tool 请求必须执行并同步回调一次。",
        failureCount);
}

HostVolumeTransferFunction GetVolumeTransferFunction()
{
    HostVolumeTransferFunction function;
    function.colorNodes = {
        { -1000.0, 0.0, 0.0, 0.0 },
        { 500.0, 0.8, 0.6, 0.4 },
        { 3000.0, 1.0, 1.0, 1.0 }
    };
    function.opacityNodes = {
        { -1000.0, 0.0 },
        { 250.0, 0.2 },
        { 3000.0, 1.0 }
    };
    return function;
}

void StartTransferCases(int& failureCount)
{
    Fixture fixture;
    const auto state = fixture.GetService();
    auto function = GetVolumeTransferFunction();

    HostViewSetRequest request;
    request.targetView.viewId = "primary";
    request.volumeTransferFunction = function;
    SetExpect(fixture.Send(std::move(request))
            && state->GetVolumeTransferFunction().colorNodes.size() == 3
            && state->GetVolumeTransferFunction().opacityNodes.size() == 3,
        "ViewSet 必须提交唯一的完整 scalar TF 快照。",
        failureCount);

    function.colorNodes[1].g = 0.2;
    request = HostViewSetRequest{};
    request.targetView.viewId = "primary";
    request.volumeTransferFunction = function;
    SetExpect(fixture.Send(std::move(request))
            && std::abs(
                state->GetVolumeTransferFunction().colorNodes[1].g
                    - 0.2) < 1e-12,
        "新的完整 TF 快照必须整体替换。",
        failureCount);

    function.opacityNodes[1].opacity = 0.7;
    function.opacityNodes[1].scalar =
        function.opacityNodes[0].scalar;
    request = HostViewSetRequest{};
    request.targetView.viewId = "primary";
    request.volumeTransferFunction = function;
    SetExpect(!fixture.Send(std::move(request))
            && std::abs(
                state->GetVolumeTransferFunction().opacityNodes[1].opacity
                    - 0.2) < 1e-12,
        "TF 重复 scalar 必须在 Router 边界整笔拒绝。",
        failureCount);

    function = GetVolumeTransferFunction();
    function.colorNodes.resize(1);
    request = HostViewSetRequest{};
    request.targetView.viewId = "primary";
    request.volumeTransferFunction = function;
    SetExpect(!fixture.Send(std::move(request)),
        "颜色或透明度少于两个节点必须拒绝。",
        failureCount);
}

void StartContextCases(int& failureCount)
{
    Fixture fixture;
    const auto service = fixture.GetService();
    std::weak_ptr<ViewContextStub> expiredContext = fixture.context;
    fixture.context.reset();
    SetExpect(
        fixture.views->ClearViewContext("primary")
            && expiredContext.expired(),
        "Context 失效夹具必须只保留 route 中的过期 weak_ptr。",
        failureCount);

    const auto oldRevision = service->GetRevision();
    HostViewSetRequest viewRequest;
    viewRequest.targetView.viewId = "primary";
    viewRequest.mode = HostRenderMode::IsoSurface;
    viewRequest.isAxesVisible = true;
    SetExpect(
        !SendView(fixture, std::move(viewRequest))
            && service->GetViewSetCount() == 0
            && service->GetDirtySetCount() == 0
            && service->GetRevision() == oldRevision,
        "过期 Context 必须在 mode/axes 事务写入 App 前拒绝。",
        failureCount);

    HostViewResetRequest resetRequest;
    resetRequest.targetView.viewId = "primary";
    SetExpect(
        !fixture.Send(std::move(resetRequest))
            && service->GetDirtySetCount() == 0,
        "过期 Context 的 ResetCamera 不得标记渲染。",
        failureCount);

    HostToolSetRequest toolRequest;
    toolRequest.targetView.viewId = "primary";
    toolRequest.toolMode = HostToolMode::ModelTransform;
    SetExpect(!fixture.Send(std::move(toolRequest)),
        "过期 Context 的 Tool Set 必须安全拒绝。",
        failureCount);

    HostToolSwitchRequest switchRequest;
    switchRequest.targetView.viewId = "primary";
    SetExpect(!fixture.Send(std::move(switchRequest)),
        "过期 Context 的 Tool Switch 必须安全拒绝。",
        failureCount);
}

} // namespace

int HostRouterSuite::GetFailCount() const
{
    int failureCount = 0;
    StartDataCases(failureCount);
    StartViewCases(failureCount);
    StartTransferCases(failureCount);
    StartToolCases(failureCount);
    StartContextCases(failureCount);
    return failureCount;
}
