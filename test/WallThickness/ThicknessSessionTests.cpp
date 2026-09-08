#include "Host/WallThicknessHostFeature.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Host/PartSegmentationHostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "App/Services/FeatureViewService.h"
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct Acceptance final
{
    HostReloadRequest volume;
    double threshold = 500;
    ThicknessParams params;
    ThicknessEvaluation evaluation;
    ThicknessUnit unit = ThicknessUnit::Millimeter;
    std::uint64_t label = 1;
    double reference = 8, tolerance = 0.2, minCoverage = 0.5;
    bool isReal = false;
};
Acceptance ReadCase(const char *path)
{
    // 外部验收输入为严格有序文本，不推测RAW几何/单位，也不缺失时换合成数据。
    std::ifstream input(path);
    std::string schema, rawPath, unit;
    Acceptance c;
    c.isReal = true;
    input >> schema >> std::quoted(rawPath);
    Require(schema == "wall-thickness-acceptance-1", "Invalid acceptance schema.");
    for (auto &v : c.volume.geometry.dimensions)
        input >> v;
    for (auto &v : c.volume.geometry.spacing)
        input >> v;
    for (auto &v : c.volume.geometry.origin)
        input >> v;
    for (auto &v : c.volume.geometry.direction)
        input >> v;
    input >> unit >> c.threshold >> c.label;
    c.unit = unit == "mm"  ? ThicknessUnit::Millimeter
             : unit == "m" ? ThicknessUnit::Meter
                           : ThicknessUnit::Unknown;
    auto &p = c.params;
    input >> p.maxDistance >> p.sampleSpacing >> p.reverseTolerance >> p.maxFitResidual >>
        p.maxLocalizationSigma >> p.minSupportRatio >> p.maxBoundaryError;
    p.directionCount = 1;
    std::array<double, 6> bounds{};
    for (auto &v : bounds)
        input >> v;
    p.evaluationBounds = bounds;
    input >> c.evaluation.lower >> c.evaluation.upper >> c.reference >> c.tolerance >>
        c.minCoverage;
    Require(bool(input) && c.unit == ThicknessUnit::Millimeter && std::isfinite(c.reference) &&
                std::isfinite(c.tolerance) && c.tolerance >= 0 && std::isfinite(c.minCoverage) &&
                c.minCoverage > 0 && c.minCoverage <= 1,
            "Acceptance requires explicit LPS geometry in mm, recipe and reference.");
    std::string extra;
    Require(!(input >> extra), "Unexpected acceptance fields.");
    std::size_t count = 1;
    for (auto d : c.volume.geometry.dimensions)
    {
        Require(d > 0 && count <= std::size_t(512) * 1024 * 1024 / static_cast<std::size_t>(d),
                "Acceptance volume budget exceeded.");
        count *= static_cast<std::size_t>(d);
    }
    c.volume.voxels.resize(count);
    std::ifstream raw(rawPath, std::ios::binary | std::ios::ate);
    Require(raw && raw.tellg() == static_cast<std::streamoff>(count * sizeof(float)),
            "RAW float32 little-endian shape mismatch.");
    raw.seekg(0);
    raw.read(reinterpret_cast<char *>(c.volume.voxels.data()),
             static_cast<std::streamsize>(count * sizeof(float)));
    Require(bool(raw), "RAW read failed.");
    c.volume.metadata.identity.datasetId = rawPath;
    c.volume.metadata.source.uri = rawPath;
    c.volume.metadata.source.kind = ImageSourceKind::Memory;
    c.evaluation.histogramRange = {0, p.maxDistance};
    return c;
}
Acceptance Synthetic()
{
    Acceptance c;
    c.volume.geometry.dimensions = {32, 32, 32};
    c.volume.geometry.spacing = {1, 1, 1};
    c.volume.geometry.origin = {-31, -31, 0};
    c.volume.voxels.resize(32 * 32 * 32);
    c.volume.metadata.identity.datasetId = "synthetic-rounded-slab";
    c.volume.metadata.source.kind = ImageSourceKind::Memory;
    c.volume.metadata.source.uri = "memory://synthetic-rounded-slab";
    for (int z = 0; z < 32; ++z)
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                const double distance = std::max(
                    {std::abs(x - 15.5) - 10, std::abs(y - 15.5) - 10, std::abs(z - 15.5) - 4});
                c.volume.voxels[x + 32 * (y + 32 * z)] =
                    static_cast<float>(500 * (1 - std::tanh(distance / 0.7)));
            }
    auto &p = c.params;
    p.maxDistance = 24;
    p.sampleSpacing = 1;
    p.reverseTolerance = 0.25;
    // 合成灰度跨度1000；允许3%线性拟合残差，定位稳定性另以物理长度约束。
    p.maxFitResidual = 30;
    p.maxLocalizationSigma = 0.2;
    p.minSupportRatio = 0.5;
    p.maxBoundaryError = 0.5;
    p.directionCount = 1;
    p.evaluationBounds = std::array<double, 6>{10, 21, 10, 21, 0, 31};
    c.evaluation = {7, 9, {0, 24}, 24, 0};
    return c;
}
class Probe final : public HostFeature
{
  public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "thickness.acceptance.probe";
    }
    bool AttachHost(const HostFeatureContext &c) override
    {
        context = c;
        return true;
    }
    bool DetachHost() override
    {
        context = {};
        return true;
    }
    bool OnHostTick() override
    {
        return true;
    }
    HostFeatureContext context;
};
void TestSession(Acceptance c)
{
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    const std::array<HostRenderViewRole, 4> roles{
        HostRenderViewRole::Primary3D, HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice};
    const std::array<HostRenderMode, 4> modes{
        HostRenderMode::IsoSurface, HostRenderMode::SliceTopDown, HostRenderMode::SliceFrontBack,
        HostRenderMode::SliceLeftRight};
    for (std::size_t i = 0; i < 4; ++i)
    {
        HostRenderViewConfig view;
        view.id = "thickness-" + std::to_string(i);
        view.role = roles[i];
        view.inputMode = HostInputMode::HostInjected;
        view.isEventLoopEnabled = i == 0;
        view.window.width = 256;
        view.window.height = 256;
        view.window.viewInit.viewMode = modes[i];
        view.window.viewInit.hasIso = true;
        view.window.viewInit.isoThreshold = c.threshold;
        view.renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        view.renderWindow->SetOffScreenRendering(1);
        config.renderViews.push_back(view);
    }
    VtkAppHostSession session(std::move(config));
    struct StopGuard final
    {
        VtkAppHostSession &session;
        ~StopGuard()
        {
            session.Stop();
        }
    } stopGuard{session};
    Require(session.BuildSession(), "BuildSession failed.");
    auto probe = std::make_shared<Probe>();
    auto part = std::make_shared<PartSegmentationHostFeature>();
    auto surface = std::make_shared<SurfaceDeterminationHostFeature>();
    auto wall = std::make_shared<WallThicknessHostFeature>();
    Require(session.AttachFeature(probe) && session.AttachFeature(part) &&
                session.AttachFeature(surface) && session.AttachFeature(wall),
            "Attach public features failed.");
    Require(!session.Start(), "HostDriven must reject the native event-loop entry.");
    const auto wait = [&](const auto &ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
        {
            session.SendUpdates();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(ready(), "Public entry completion deadline exceeded.");
    };
    bool loaded = false;
    Require(session.SendRequest(std::move(c.volume), [&](bool ok) { loaded = ok; }),
            "Volume reload rejected.");
    wait([&] { return loaded; });
    PartSegmentationRequest pr;
    pr.action = PartSegmentationAction::Start;
    pr.start = PartSegmentationStartParams{};
    pr.start->threshold = c.threshold;
    pr.start->targetViews.viewIds = {"thickness-0"};
    std::optional<PartSegmentationResult> parts;
    Require(part->SendRequest(pr, [&](auto r) { parts = std::move(r); }).status ==
                PartAdmissionStatus::Accepted,
            "Part request rejected.");
    wait([&] { return bool(parts); });
    Require(parts->status == PartResultStatus::Succeeded, "Part segmentation failed.");
    SurfaceDeterminationRequest sr;
    sr.action = SurfaceDeterminationAction::Start;
    sr.start = SurfaceDeterminationStartParams{};
    sr.start->targetViews.viewIds = {"thickness-0"};
    sr.start->componentSelection = SurfaceComponentSelection::All;
    sr.start->initialIsoValue = c.threshold;
    sr.start->minimumObjectVoxels = 1;
    std::optional<SurfaceDeterminationResult> determined;
    Require(surface->SendRequest(sr, [&](auto r) { determined = std::move(r); }).status ==
                SurfaceAdmissionStatus::Accepted,
            "Surface request rejected.");
    wait([&] { return bool(determined); });
    Require(determined->status == SurfaceResultStatus::Succeeded, "Surface determination failed.");
    const auto mesh = surface->GetSurfaceSnapshot();
    Require(bool(mesh), "Surface snapshot missing.");
    Require(determined->isPublished && mesh->purpose == SurfaceTaskPurpose::Determine &&
                mesh->sourceRevision == parts->sourceRevision &&
                surface->GetResultValidity(mesh->dataRevision).status == SurfaceRestoreStatus::Current,
            "Wall input requires a current formal Surface generation from the Part source.");
    std::size_t accepted = 0;
    double maxResidual = 0, maxSigma = 0;
    for (const auto &point : *mesh->points)
    {
        if (point.flags == SurfacePointFlags::None)
            ++accepted;
        maxResidual = std::max(maxResidual, double(point.fitResidual));
        maxSigma = std::max(maxSigma, double(point.estimatedLocalizationSigma));
    }
    std::cout << "surface points=" << mesh->points->size() << " accepted=" << accepted
              << " fitResidualMax=" << maxResidual << " localizationSigmaMax=" << maxSigma << '\n';
    ThicknessRequest request;
    request.action = ThicknessAction::Start;
    request.input = ThicknessInput{mesh->sourceRevision,
                                   parts->labelMap,
                                   mesh->meshRevision,
                                   std::string(primaryVolumeBinding),
                                   {},
                                   {},
                                   c.label,
                                   c.unit};
    request.params = c.params;
    request.evaluation = c.evaluation;
    ThicknessDisplay display;
    display.range = {0, c.params.maxDistance};
    for (int i = 0; i < 4; ++i)
        display.targetViews.viewIds.push_back("thickness-" + std::to_string(i));
    request.display = display;
    std::optional<ThicknessResult> completed;
    Require(wall->SendRequest(request, [&](auto r) { completed = std::move(r); }).status ==
                ThicknessAdmissionStatus::Accepted,
            "Wall request rejected.");
    wait([&] { return bool(completed); });
    std::cout << "outcome=" << unsigned(completed->status) << " message=" << completed->message
              << '\n';
    const auto result = wall->GetResult(completed->result);
    if (result)
    {
        std::cout << "evaluatedArea=" << result->statistics.evaluatedArea
                  << " validArea=" << result->statistics.validArea << " reasons=";
        for (auto count : result->statistics.reasonCounts)
            std::cout << count << ',';
        std::cout << '\n';
    }
    Require(result && completed->status == ThicknessStatus::Succeeded && completed->isActivated,
            "Wall public analysis failed.");
    std::cout << std::setprecision(17) << "minimum=" << result->statistics.minimum.value_or(-1)
              << " coverage=" << result->statistics.coverage
              << " samples=" << result->statistics.sampleCount << '\n';
    Require(result->statistics.minimum &&
                std::abs(*result->statistics.minimum - c.reference) <= c.tolerance,
            "Reference minimum tolerance failed.");
    Require(result->statistics.coverage >= c.minCoverage, "Reference minimum coverage failed.");
    Require(completed->isDisplayReady, "Wall four-view display failed.");
    session.SendUpdates();
    ThicknessRequest selected;
    selected.action = ThicknessAction::SelectSample;
    selected.resultRevision = completed->result;
    selected.sampleIndex = result->statistics.minimumSample;
    Require(wall->SendRequest(selected).status == ThicknessAdmissionStatus::Accepted,
            "Sample selection failed.");
    const auto &sample = (*result->samples)[*selected.sampleIndex];
    const auto view = probe->context.views->GetFeaturePort("thickness-0");
    const auto world = view->GetWorldPosition(sample.source);
    Require(bool(world), "Sample world position missing.");
    // 上位机用现有Session请求定位切片，Feature不获得Host私有写端。
    HostSessionSetRequest cursor;
    cursor.cursor = HostCursorParams{*world, -1};
    bool positioned = false;
    Require(session.SendRequest(std::move(cursor), [&](bool ok) { positioned = ok; }),
            "Cursor request rejected.");
    wait([&] { return positioned; });
    session.SendUpdates();
    Require(session.SendRender({display.targetViews.viewIds, 0.001}).status ==
                HostRenderStatus::Rendered,
            "Actual OpenGL render failed.");
    const auto before = wall->GetResult(completed->result)->samples;
    ThicknessRequest color;
    color.action = ThicknessAction::SetDisplay;
    color.display = display;
    color.display->mode = ThicknessDisplayMode::Tolerance;
    Require(wall->SendRequest(color).status == ThicknessAdmissionStatus::Accepted,
            "Tolerance display rejected.");
    Require(wall->GetResult(completed->result)->samples == before,
            "Display changes numerical buffer.");
    Require(session.DetachFeature(*wall) && session.DetachFeature(*surface) &&
                session.DetachFeature(*part) && session.DetachFeature(*probe),
            "Public feature detach failed.");
    Require(session.Stop(), "Session Stop failed.");
    std::cout << (c.isReal ? "REAL" : "SYNTHETIC")
              << " public Host/Part/Surface/Wall/4-view/cursor pipeline passed.\n";
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--synthetic")
            TestSession(Synthetic());
        else if (argc == 3 && std::string(argv[1]) == "--real")
            TestSession(ReadCase(argv[2]));
        else
        {
            std::cerr << "Usage: WallThicknessSessionTests --synthetic | --real <acceptance.txt>\n";
            return 2;
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
