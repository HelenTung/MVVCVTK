#include "../TestDataPort.h"
#include "Host/WallThicknessHostFeature.h"
#include "ThicknessAlgorithm.h"
#include "ThicknessData.h"
#include "ThicknessOverlay.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"
#include <vtkActorCollection.h>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCell.h>
#include <vtkIdTypeArray.h>
#include <vtkLegendBoxActor.h>
#include <vtkPolyDataMapper.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkPNGWriter.h>
#include <vtkWindowToImageFilter.h>
#include <vtkSphereSource.h>
#include <vtkIdList.h>
#include <vtkCellArray.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace
{
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
ThicknessAlgorithm::Work BuildSlab(double origin = 0.0, double scale = 1.0)
{
    GridGeometry3D g;
    g.extent = {0, 7, 0, 7, 0, 7};
    g.dimensions = {8, 8, 8};
    g.origin = {origin, origin, origin};
    g.spacing = {scale, scale, scale};
    auto labels = std::make_shared<std::vector<std::uint64_t>>(512, 0);
    for (int z = 2; z <= 3; ++z)
        for (int y = 2; y <= 5; ++y)
            for (int x = 2; x <= 5; ++x)
                (*labels)[x + 8 * (y + 8 * z)] = 1;
    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(512, 100);
    std::vector<double> vertices;
    const std::array<ThicknessPoint, 8> points{{{1.5, 1.5, 1.5},
                                                {5.5, 1.5, 1.5},
                                                {5.5, 5.5, 1.5},
                                                {1.5, 5.5, 1.5},
                                                {1.5, 1.5, 3.5},
                                                {5.5, 1.5, 3.5},
                                                {5.5, 5.5, 3.5},
                                                {1.5, 5.5, 3.5}}};
    for (const auto &p : points)
        for (double v : p)
            vertices.push_back(origin + v * scale);
    std::vector<std::uint64_t> triangles{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                         1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    std::vector<double> normals;
    for (int i = 0; i < 8; ++i)
        normals.insert(normals.end(), {0, 0, i < 4 ? -1.0 : 1.0});
    std::vector<MeshAttribute> attributes{
        {"measurement.valid", 1, std::vector<double>(8, 1)},
        {"measurement.normal", 3, normals},
        {"measurement.fit-residual", 1, std::vector<double>(8, 0)},
        {"measurement.support-ratio", 1, std::vector<double>(8, 1)},
        {"measurement.localization-sigma", 1, std::vector<double>(8, 0)},
        {"measurement.boundary-complete", 1, std::vector<double>(8, 1)}};
    ThicknessAlgorithm::Work w;
    w.source = std::make_shared<const ImageGrid3DPayload>(g, ImageValueType::UInt8, 1, bytes);
    w.labels = std::make_shared<const LabelMap3DPayload>(
        g, LabelMapValues{std::shared_ptr<const std::vector<std::uint64_t>>(labels)});
    w.mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices), std::move(triangles),
                                                        std::move(attributes));
    w.archive.input = {
        GetTestDataRef(1),        GetTestDataRef(2), GetTestDataRef(3), {}, {}, {}, 1,
        ThicknessUnit::Millimeter};
    auto &p = w.archive.params;
    p.maxDistance = 10 * scale;
    p.sampleSpacing = scale;
    p.reverseTolerance = 1e-5 * scale;
    p.directionCount = 1;
    p.maxFitResidual = scale;
    p.maxLocalizationSigma = scale;
    w.archive.evaluation = {1.5 * scale, 2.5 * scale, {0, 4 * scale}, 8, 0};
    w.cancelled = std::make_shared<std::atomic<bool>>(false);
    w.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    return w;
}
ThicknessAlgorithm::Work BuildShell(bool hasInner = true)
{
    auto w = BuildSlab();
    GridGeometry3D g;
    g.extent = {0, 11, 0, 11, 0, 11};
    g.dimensions = {12, 12, 12};
    auto labels = std::make_shared<std::vector<std::uint8_t>>(1728, 0);
    for (int z = 0; z < 12; ++z)
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x)
            {
                const double radius = std::hypot(x - 5.5, y - 5.5, z - 5.5);
                (*labels)[x + 12 * (y + 12 * z)] = radius >= 2 && radius <= 4 ? 1 : 0;
            }
    w.labels = std::make_shared<const LabelMap3DPayload>(
        g, LabelMapValues{std::shared_ptr<const std::vector<std::uint8_t>>(labels)});
    w.source = std::make_shared<const ImageGrid3DPayload>(
        g, ImageValueType::UInt8, 1, std::make_shared<const std::vector<std::uint8_t>>(1728, 100));
    std::vector<double> vertices, normals;
    std::vector<std::uint64_t> triangles;
    for (int shell = 0; shell < (hasInner ? 2 : 1); ++shell)
    {
        auto sphere = vtkSmartPointer<vtkSphereSource>::New();
        sphere->SetCenter(5.5, 5.5, 5.5);
        sphere->SetRadius(shell ? 2 : 4);
        sphere->SetThetaResolution(32);
        sphere->SetPhiResolution(32);
        sphere->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);
        sphere->Update();
        auto *mesh = sphere->GetOutput();
        const auto offset = vertices.size() / 3;
        for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i)
        {
            double p[3]{};
            mesh->GetPoint(i, p);
            for (int k = 0; k < 3; ++k)
            {
                vertices.push_back(p[k]);
                normals.push_back((p[k] - 5.5) / (shell ? 2 : 4));
            }
        }
        auto ids = vtkSmartPointer<vtkIdList>::New();
        mesh->GetPolys()->InitTraversal();
        while (mesh->GetPolys()->GetNextCell(ids))
            for (vtkIdType k = 0; k < 3; ++k)
                triangles.push_back(offset + ids->GetId(shell ? 2 - k : k));
    }
    const auto count = vertices.size() / 3;
    w.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(triangles),
        std::vector<MeshAttribute>{
            {"measurement.valid", 1, std::vector<double>(count, 1)},
            {"measurement.normal", 3, std::move(normals)},
            {"measurement.fit-residual", 1, std::vector<double>(count, 0)},
            {"measurement.support-ratio", 1, std::vector<double>(count, 1)},
            {"measurement.localization-sigma", 1, std::vector<double>(count, 0)},
            {"measurement.boundary-complete", 1, std::vector<double>(count, 1)}});
    w.archive.params.maxBoundaryError = 0.5;
    w.archive.params.reverseTolerance = 0.1;
    return w;
}
void TestRayDefinitions()
{
    auto slab = BuildSlab();
    slab.archive.params.directionCount = 9;
    slab.archive.params.reverseTolerance = 1;
    slab.archive.params.evaluationBounds = std::array<double, 6>{2.5, 4.5, 2.5, 4.5, 0, 7};
    auto rays = ThicknessAlgorithm::BuildField(slab);
    Check(rays.status == ThicknessStatus::Succeeded && rays.statistics.minimum &&
              std::abs(*rays.statistics.minimum - 2) < 1e-8,
          "cone candidates retain the normal thickness");
    slab.archive.params.ambiguityRelative = 0;
    slab.archive.params.ambiguityAbsolute = 0;
    auto ambiguous = ThicknessAlgorithm::BuildField(slab);
    Check(ambiguous.status == ThicknessStatus::NoValidSamples &&
              ambiguous.statistics.reasonCounts[static_cast<std::size_t>(
                  ThicknessValidity::AmbiguousOpposite)] > 0,
          "conflicting direction distances are not silently reduced to a minimum");
    auto samples = std::make_shared<std::vector<ThicknessSample>>(4);
    const std::array<double, 4> values{1, 2, 9, 0}, areas{9, 1, 1, 100};
    for (std::size_t i = 0; i < 4; ++i)
    {
        (*samples)[i].thickness = values[i];
        (*samples)[i].area = areas[i];
        (*samples)[i].validity =
            i == 3 ? ThicknessValidity::LowSurfaceQuality : ThicknessValidity::Valid;
    }
    auto neighbors = std::make_shared<ThicknessAlgorithm::Neighbors>(
        4,
        std::array<std::size_t, 3>{ThicknessAlgorithm::noNeighbor, ThicknessAlgorithm::noNeighbor,
                                   ThicknessAlgorithm::noNeighbor});
    auto weighted = ThicknessAlgorithm::BuildEvaluation(
        {samples, neighbors, 1}, slab.archive.evaluation, slab.archive.params, slab.archive.limits,
        slab.cancelled, slab.deadline);
    Check(weighted.statistics.mean && std::abs(*weighted.statistics.mean - 20.0 / 11) < 1e-12 &&
              weighted.statistics.quantiles[1] && *weighted.statistics.quantiles[1] == 1 &&
              std::abs(weighted.statistics.coverage - 11.0 / 111) < 1e-12,
          "mean, median and coverage use area rather than vertex density");
    auto tube = BuildSlab();
    GridGeometry3D g;
    g.extent = {0, 11, 0, 11, 0, 11};
    g.dimensions = {12, 12, 12};
    auto labels = std::make_shared<std::vector<std::uint8_t>>(1728, 0);
    for (int z = 0; z < 12; ++z)
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x)
            {
                const double radius = std::hypot(x - 5.5, y - 5.5);
                (*labels)[x + 12 * (y + 12 * z)] =
                    radius >= 2 && radius <= 4 && z >= 4 && z <= 7 ? 1 : 0;
            }
    tube.labels = std::make_shared<const LabelMap3DPayload>(
        g, LabelMapValues{std::shared_ptr<const std::vector<std::uint8_t>>(labels)});
    tube.source = std::make_shared<const ImageGrid3DPayload>(
        g, ImageValueType::UInt8, 1, std::make_shared<const std::vector<std::uint8_t>>(1728, 100));
    constexpr std::size_t segments = 48;
    std::vector<double> vertices, normals;
    std::vector<std::uint64_t> triangles;
    for (int ring = 0; ring < 4; ++ring)
        for (std::size_t i = 0; i < segments; ++i)
        {
            const double angle = 2 * 3.14159265358979323846 * i / segments;
            const double radius = ring < 2 ? 4 : 2, z = ring % 2 ? 7.5 : 3.5;
            vertices.insert(vertices.end(),
                            {5.5 + radius * std::cos(angle), 5.5 + radius * std::sin(angle), z});
            normals.insert(normals.end(), {std::cos(angle), std::sin(angle), 0});
        }
    for (std::size_t i = 0; i < segments; ++i)
    {
        const auto j = (i + 1) % segments, ob = i, obn = j, ot = i + segments, otn = j + segments;
        const auto ib = i + 2 * segments, ibn = j + 2 * segments, it = i + 3 * segments,
                   itn = j + 3 * segments;
        const std::array<std::uint64_t, 24> faces{ob,  obn, otn, ob,  otn, ot, ib,  itn,
                                                  ibn, ib,  it,  itn, ob,  ib, ibn, ob,
                                                  ibn, obn, ot,  itn, it,  ot, otn, itn};
        triangles.insert(triangles.end(), faces.begin(), faces.end());
    }
    const auto count = vertices.size() / 3;
    tube.mesh = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(triangles),
        std::vector<MeshAttribute>{
            {"measurement.valid", 1, std::vector<double>(count, 1)},
            {"measurement.normal", 3, std::move(normals)},
            {"measurement.fit-residual", 1, std::vector<double>(count, 0)},
            {"measurement.support-ratio", 1, std::vector<double>(count, 1)},
            {"measurement.localization-sigma", 1, std::vector<double>(count, 0)},
            {"measurement.boundary-complete", 1, std::vector<double>(count, 1)}});
    tube.archive.params.maxBoundaryError = 0.5;
    tube.archive.params.reverseTolerance = 0.1;
    const auto wall = ThicknessAlgorithm::BuildField(tube);
    Check(wall.status == ThicknessStatus::Succeeded && wall.statistics.minimum &&
              std::abs(*wall.statistics.minimum - 2) < 0.05,
          "closed tube measures radial wall instead of void diameter");
}

void Algorithm()
{
    TestRayDefinitions();
    auto shell = ThicknessAlgorithm::BuildField(BuildShell());
    Check(shell.status == ThicknessStatus::Succeeded && shell.statistics.minimum &&
              std::abs(*shell.statistics.minimum - 2) < 0.05,
          "closed sphere shell measures wall, not diameter");
    Check(ThicknessAlgorithm::BuildField(BuildShell(false)).status ==
              ThicknessStatus::IncompleteBoundary,
          "missing cavity rejected even with a complete-boundary declaration");
    auto w = BuildSlab();
    auto result = ThicknessAlgorithm::BuildField(w);
    Check(result.status == ThicknessStatus::Succeeded, "slab analysis succeeds");
    if (result.status != ThicknessStatus::Succeeded)
    {
        std::cerr << result.message << '\n';
        return;
    }
    Check(result.statistics.minimum && std::abs(*result.statistics.minimum - 2) < 1e-8,
          "slab minimum=2");
    Check(result.statistics.maximum && std::abs(*result.statistics.maximum - 2) < 1e-8,
          "slab maximum=2");
    Check(result.statistics.validCount > 0 && result.statistics.coverage < 1,
          "invalid side normals reduce coverage");
    Check(std::abs(result.statistics.evaluatedArea - 64) < 1e-8,
          "both-side total area includes invalid sides");
    auto rotated = BuildSlab();
    auto g = rotated.labels->GetGeometry();
    g.extent = {-4, 3, 8, 15, 2, 9};
    g.spacing = {1, 2, 3};
    g.origin = {10, 20, 30};
    g.direction = {0, -1, 0, 1, 0, 0, 0, 0, 1};
    auto vertices = rotated.mesh->GetVertices();
    for (std::size_t i = 0; i < vertices.size(); i += 3)
    {
        const double x = vertices[i] - 4, y = vertices[i + 1] + 8, z = vertices[i + 2] + 2;
        vertices[i] = 10 - 2 * y;
        vertices[i + 1] = 20 + x;
        vertices[i + 2] = 30 + 3 * z;
    }
    rotated.labels = std::make_shared<const LabelMap3DPayload>(g, rotated.labels->GetValues());
    rotated.source = std::make_shared<const ImageGrid3DPayload>(g, ImageValueType::UInt8, 1,
                                                                rotated.source->GetValues());
    rotated.mesh = std::make_shared<const SurfaceMeshPayload>(
        vertices, rotated.mesh->GetTriangles(), rotated.mesh->GetPointAttributes());
    rotated.archive.params.maxDistance = 30;
    rotated.archive.params.sampleSpacing = 3;
    rotated.archive.evaluation.upper = 10;
    const auto oriented = ThicknessAlgorithm::BuildField(rotated);
    Check(oriented.status == ThicknessStatus::Succeeded && oriented.statistics.minimum &&
              std::abs(*oriented.statistics.minimum - 6) < 1e-7,
          "anisotropic rotated nonzero extent uses physical lengths");
    auto wide = BuildSlab();
    const auto hugeLabel = std::numeric_limits<std::uint64_t>::max() - 2;
    auto integers = std::make_shared<std::vector<std::uint64_t>>(512, 0);
    for (int z = 2; z <= 3; ++z)
        for (int y = 2; y <= 5; ++y)
            for (int x = 2; x <= 5; ++x)
                (*integers)[x + 8 * (y + 8 * z)] = hugeLabel;
    wide.labels = std::make_shared<const LabelMap3DPayload>(
        wide.labels->GetGeometry(),
        LabelMapValues{std::shared_ptr<const std::vector<std::uint64_t>>(integers)});
    wide.archive.input.materialLabel = hugeLabel;
    const auto exactLabel = ThicknessAlgorithm::BuildField(wide);
    Check(exactLabel.status == ThicknessStatus::Succeeded &&
              exactLabel.statistics.validCount == result.statistics.validCount,
          "uint64 material identity never rounds through double");
    auto large = ThicknessAlgorithm::BuildField(BuildSlab(1e8, 0.001));
    Check(large.status == ThicknessStatus::Succeeded && large.statistics.minimum &&
              std::abs(*large.statistics.minimum - 0.002) < 5e-8,
          "large origin preserves small thickness");
    auto attrs = w.mesh->GetPointAttributes();
    attrs.back().values.assign(8, 0);
    w.mesh = std::make_shared<const SurfaceMeshPayload>(w.mesh->GetVertices(),
                                                        w.mesh->GetTriangles(), attrs);
    Check(ThicknessAlgorithm::BuildField(w).status == ThicknessStatus::IncompleteBoundary,
          "Largest provenance rejected");
    w = BuildSlab();
    auto triangles = w.mesh->GetTriangles();
    triangles.resize(triangles.size() - 3);
    w.mesh = std::make_shared<const SurfaceMeshPayload>(w.mesh->GetVertices(), triangles,
                                                        w.mesh->GetPointAttributes());
    Check(ThicknessAlgorithm::BuildField(w).status == ThicknessStatus::IncompleteBoundary,
          "open mesh rejected");
    w = BuildSlab();
    w.cancelled->store(true);
    Check(ThicknessAlgorithm::BuildField(w).status == ThicknessStatus::Cancelled,
          "cancel before work");
    w = BuildSlab();
    w.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    Check(ThicknessAlgorithm::BuildField(w).status == ThicknessStatus::DeadlineExceeded,
          "absolute deadline");
    w = BuildSlab();
    w.archive.limits.maxSamples = 1;
    Check(ThicknessAlgorithm::BuildField(w).status == ThicknessStatus::BudgetExceeded,
          "no hidden sample downgrade");
    w = BuildSlab();
    auto labels = std::make_shared<std::vector<std::uint64_t>>(512, 0);
    for (int z = 2; z <= 3; ++z)
        for (int y = 2; y <= 5; ++y)
            for (int x = 2; x <= 5; ++x)
                (*labels)[x + 8 * (y + 8 * z)] = 1;
    (*labels)[3 + 8 * (3 + 8 * 2)] = 2;
    w.labels = std::make_shared<const LabelMap3DPayload>(
        w.labels->GetGeometry(),
        LabelMapValues{std::shared_ptr<const std::vector<std::uint64_t>>(labels)});
    const auto foreign = ThicknessAlgorithm::BuildField(w);
    Check(foreign.status == ThicknessStatus::IncompleteBoundary ||
              (foreign.status == ThicknessStatus::Succeeded &&
               foreign.statistics.validCount < result.statistics.validCount),
          "foreign material is rejected as missing interface or invalid path");
}
void Evaluation()
{
    auto w = BuildSlab();
    auto result = ThicknessAlgorithm::BuildField(w);
    auto evaluation = w.archive.evaluation;
    evaluation.lower = 2.1;
    evaluation.upper = 3;
    auto changed = ThicknessAlgorithm::BuildEvaluation(result.field, evaluation, w.archive.params,
                                                       w.archive.limits, w.cancelled, w.deadline);
    Check(changed.status == ThicknessStatus::Succeeded && !changed.regions.empty(),
          "thin regions found");
    Check(changed.statistics.minimum && *changed.statistics.minimum > 1.99,
          "invalid zero excluded");
    Check(changed.statistics.quantiles[1] && std::abs(*changed.statistics.quantiles[1] - 2) < 1e-8,
          "weighted median");
    Check(changed.field.samples == nullptr,
          "evaluation does not manufacture a replacement numerical field");
    auto samples = std::make_shared<std::vector<ThicknessSample>>(*result.field.samples);
    for (auto &sample : *samples)
    {
        sample.validity = ThicknessValidity::LowSurfaceQuality;
        sample.thickness = 0;
    }
    ThicknessAlgorithm::Field invalid{samples, result.field.neighbors, result.field.subdivisions};
    auto empty = ThicknessAlgorithm::BuildEvaluation(invalid, evaluation, w.archive.params,
                                                     w.archive.limits, w.cancelled, w.deadline);
    Check(empty.status == ThicknessStatus::NoValidSamples && !empty.statistics.minimum &&
              empty.statistics.coverage == 0,
          "all invalid has no fabricated minimum");
}
class Control final : public FeatureHostControl
{
  public:
    bool AttachInput(HostInputBinding input) override
    {
        binding = std::move(input);
        if (isThrowAttach)
            throw std::runtime_error("Injected attach failure");
        return true;
    }
    bool DetachInput(std::string_view) override
    {
        binding = {};
        return true;
    }
    bool SetActiveViews(const std::vector<std::string> &) override
    {
        return true;
    }
    bool SetViewStatus(const std::vector<std::string> &, const std::string &) override
    {
        return true;
    }
    bool SendSceneDelta(FeatureSceneDelta value) override
    {
        delta = std::move(value);
        return true;
    }
    bool SendOwnerComplete(std::function<void()> complete) override
    {
        complete();
        return true;
    }
    bool SendWorkAvailable() override
    {
        return true;
    }
    std::uint64_t GetAttachmentId() const noexcept override
    {
        return 1;
    }
    bool isThrowAttach = false;
    HostInputBinding binding;
    FeatureSceneDelta delta;
};
ThicknessInput Publish(const std::shared_ptr<TestDataPort> &data,
                       const ThicknessAlgorithm::Work &work)
{
    ThicknessInput input = work.archive.input;
    input.source = {data->CreateDataEntityId(), 1};
    input.labels = {data->CreateDataEntityId(), 1};
    input.mesh = {data->CreateDataEntityId(), 1};
    DataTransaction transaction;
    transaction.outputs = {{input.source.entityId, 0, DataTypes::imageGrid3D, {}, work.source, {}},
                           {input.labels.entityId,
                            0,
                            DataTypes::labelMap3D,
                            {{"source-volume", input.source}},
                            work.labels,
                            {}},
                           {input.mesh.entityId,
                            0,
                            DataTypes::surfaceMesh,
                            {{"source-volume", input.source}},
                            work.mesh,
                            {}}};
    const auto old = data->GetDataBinding(data->GetDataGraph(), primaryVolumeBinding)
                         .value_or(DataBinding{std::string(primaryVolumeBinding), {}, 0});
    transaction.bindings.push_back(
        {std::string(primaryVolumeBinding), old.revision, true, old.target, input.source});
    Check(data->SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded,
          "publish fixture input");
    return input;
}
void Wait(const std::shared_ptr<WallThicknessHostFeature> &feature, int &completions)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (completions == 0 && std::chrono::steady_clock::now() < deadline)
    {
        feature->OnHostTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(completions == 1, "exactly one completion");
}
class CountedData final : public TestDataPort
{
  public:
    DataObserverId AttachDataChange(DataChangeCallback callback) override
    {
        const auto id = TestDataPort::AttachDataChange(std::move(callback));
        if (id)
            ++count;
        return id;
    }
    bool DetachDataChange(DataObserverId id) override
    {
        const bool removed = TestDataPort::DetachDataChange(id);
        if (removed)
            --count;
        return removed;
    }
    int count = 0;
};
void Lifecycle()
{
    auto data = std::make_shared<TestDataPort>();
    auto control = std::make_shared<Control>();
    auto feature = std::make_shared<WallThicknessHostFeature>();
    auto work = BuildSlab();
    ThicknessRequest request;
    request.action = ThicknessAction::Start;
    request.input = Publish(data, work);
    request.params = work.archive.params;
    request.evaluation = work.archive.evaluation;
    Check(feature->SendRequest(request).status == ThicknessAdmissionStatus::Unavailable,
          "unattached rejected");
    Check(feature->AttachHost({{}, {}, data, control}), "attach headless feature");
    Check(!feature->AttachHost({{}, {}, data, control}), "duplicate attach rejected");
    int completions = 0;
    ThicknessResult outcome;
    Check(feature->SendRequest(request,
                               [&](const auto &r)
                               {
                                   ++completions;
                                   outcome = r;
                               })
                  .status == ThicknessAdmissionStatus::Accepted,
          "start accepted");
    Check(feature->SendRequest(request).status == ThicknessAdmissionStatus::Busy,
          "bounded single task");
    Wait(feature, completions);
    Check(outcome.status == ThicknessStatus::Succeeded && outcome.isActivated,
          "numerical result retained without view");
    const auto snapshot = feature->GetResult(outcome.result);
    Check(snapshot && snapshot->isCurrent && snapshot->statistics.minimum,
          "read immutable public result");
    const auto archive = feature->GetArchive(outcome.result);
    Check(archive && archive->params.maxDistance == request.params->maxDistance,
          "complete recipe recoverable");
    ThicknessRequest eval;
    eval.action = ThicknessAction::SetEvaluation;
    eval.evaluation = work.archive.evaluation;
    eval.evaluation->lower = 2.1;
    eval.evaluation->upper = 3;
    completions = 0;
    feature->SendRequest(eval,
                         [&](const auto &r)
                         {
                             ++completions;
                             outcome = r;
                         });
    Wait(feature, completions);
    const auto second = feature->GetResult(outcome.result);
    Check(second && snapshot && second->samples == snapshot->samples,
          "reevaluation shares immutable numerical buffer");
    Publish(data, work);
    feature->OnHostTick();
    Check(!feature->GetState().isCurrent, "new source identity invalidates historical result");
    ThicknessRequest stalePick;
    stalePick.action = ThicknessAction::SelectSample;
    stalePick.resultRevision = outcome.result;
    stalePick.sampleIndex = 0;
    Check(feature->SendRequest(stalePick).status != ThicknessAdmissionStatus::Accepted,
          "stale sample selection rejected");
    Check(feature->GetResult(outcome.result) && !feature->GetResult(outcome.result)->isCurrent,
          "history still readable");
    request.input = Publish(data, work);
    completions = 0;
    feature->SendRequest(request,
                         [&](const auto &r)
                         {
                             ++completions;
                             outcome = r;
                         });
    ThicknessRequest cancel;
    cancel.action = ThicknessAction::Cancel;
    Check(feature->SendRequest(cancel).status == ThicknessAdmissionStatus::Accepted,
          "cancel admitted");
    Wait(feature, completions);
    Check(outcome.status == ThicknessStatus::Cancelled, "cancel completes once");
    Check(feature->DetachHost() && feature->DetachHost(), "detach idempotent");
    auto counted = std::make_shared<CountedData>();
    auto failing = std::make_shared<Control>();
    failing->isThrowAttach = true;
    auto rejected = std::make_shared<WallThicknessHostFeature>();
    Check(!rejected->AttachHost({{}, {}, counted, failing}) && counted->count == 0 &&
              failing->binding.featureId.empty(),
          "throwing input attach reverses observer and partial input registration");
    Check(feature->AttachHost({{}, {}, data, control}), "reattach for observer reentry");
    request.input = Publish(data, work);
    completions = 0;
    bool observerDetached = false;
    const auto observer = data->AttachDataChange(
        [&](const DataChangeSet &change)
        {
            for (const auto &ref : change.published)
            {
                const auto record = data->GetData(data->GetDataGraph(), ref);
                if (record && record->type == ThicknessData::resultType && !observerDetached)
                {
                    observerDetached = feature->DetachHost();
                    Check(completions == 1, "Detach completes finishing request before returning");
                }
            }
        });
    Check(feature->SendRequest(request,
                               [&](const auto &r)
                               {
                                   ++completions;
                                   outcome = r;
                               })
                  .status == ThicknessAdmissionStatus::Accepted,
          "observer reentry request accepted");
    Wait(feature, completions);
    Check(observerDetached && outcome.status == ThicknessStatus::Cancelled &&
              GetDataRevisionRefValid(outcome.result),
          "observer detach retains committed identity and cancels exactly once");
    Check(data->DetachDataChange(observer), "test observer cleanup");
    Check(feature->AttachHost({{}, {}, data, control}), "reattach for callback reentry");
    request.input = Publish(data, work);
    completions = 0;
    bool queriedIdle = false;
    feature->SendRequest(request,
                         [&](const auto &)
                         {
                             ++completions;
                             queriedIdle = !feature->GetState().isBusy;
                             ThicknessRequest clear;
                             clear.action = ThicknessAction::Clear;
                             Check(feature->SendRequest(clear).status ==
                                       ThicknessAdmissionStatus::Accepted,
                                   "callback can clear published result");
                         });
    Wait(feature, completions);
    Check(queriedIdle && !GetDataRevisionRefValid(feature->GetState().result),
          "callback sees completed state");
    Check(feature->DetachHost(), "callback reentry cleanup");
    Check(feature->AttachHost({{}, {}, data, control}), "attach for post-commit conflict");
    request.input = Publish(data, work);
    completions = 0;
    feature->SendRequest(request,
                         [&](const auto &r)
                         {
                             ++completions;
                             outcome = r;
                         });
    Wait(feature, completions);
    const auto savedResult = outcome.result;
    bool restored = false;
    const auto restoreObserver = data->AttachDataChange(
        [&](const DataChangeSet &change)
        {
            for (const auto &b : change.bindings)
                if (b.binding == ThicknessData::bindingName && !b.current.target && !restored)
                {
                    restored = true;
                    DataTransaction t;
                    t.bindings.push_back({std::string(ThicknessData::bindingName),
                                          b.current.revision,
                                          true,
                                          {},
                                          savedResult});
                    Check(data->SetDataCommit(std::move(t)).status == DataCommitStatus::Succeeded,
                          "observer restores binding");
                }
        });
    ThicknessRequest clear;
    clear.action = ThicknessAction::Clear;
    ThicknessResult controlled;
    Check(feature->SendRequest(clear, [&](const auto &r) { controlled = r; }).status ==
              ThicknessAdmissionStatus::Accepted,
          "clear accepted before observer conflict");
    Check(restored && controlled.status == ThicknessStatus::StaleInput && !controlled.isActivated,
          "clear reports its superseded post-commit binding");
    data->DetachDataChange(restoreObserver);
    Check(feature->SendRequest(clear).status == ThicknessAdmissionStatus::Accepted,
          "clear before activation conflict");
    bool erased = false;
    const auto eraseObserver = data->AttachDataChange(
        [&](const DataChangeSet &change)
        {
            for (const auto &b : change.bindings)
                if (b.binding == ThicknessData::bindingName && b.current.target && !erased)
                {
                    erased = true;
                    DataTransaction t;
                    t.bindings.push_back({std::string(ThicknessData::bindingName),
                                          b.current.revision,
                                          true,
                                          b.current.target,
                                          {}});
                    Check(data->SetDataCommit(std::move(t)).status == DataCommitStatus::Succeeded,
                          "observer supersedes activation");
                }
        });
    ThicknessRequest activate;
    activate.action = ThicknessAction::SetActive;
    activate.resultRevision = savedResult;
    Check(feature->SendRequest(activate, [&](const auto &r) { controlled = r; }).status ==
              ThicknessAdmissionStatus::Accepted,
          "activation accepted before observer conflict");
    Check(erased && controlled.status == ThicknessStatus::StaleInput &&
              controlled.result == savedResult && !controlled.isActivated,
          "activation reports requested identity and superseded binding");
    data->DetachDataChange(eraseObserver);
    Check(feature->DetachHost(), "post-commit conflict cleanup");
}
void Display()
{
    auto w = BuildSlab();
    auto candidate = ThicknessAlgorithm::BuildField(w);
    ThicknessData::Record record{
        w.archive, candidate.field, candidate.statistics, candidate.regions, {}};
    ThicknessDisplay display;
    display.range = {0, 4};
    auto prepared = ThicknessOverlay::BuildData(record, *w.mesh, display);
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto overlay = std::make_shared<ThicknessOverlay>(prepared, display, w.archive.input.unit,
                                                      HostRenderViewRole::Primary3D);
    overlay->AttachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems() == 4,
          "feature owns surface, path, scale and invalid swatch");
    auto *colors = prepared.mesh->GetCellData()->GetScalars();
    Check(colors && colors->GetNumberOfComponents() == 3,
          "cell RGB prevents invalid interpolation");
    bool grey = false, colored = false;
    for (vtkIdType i = 0; i < colors->GetNumberOfTuples(); ++i)
    {
        double rgb[3]{};
        colors->GetTuple(i, rgb);
        grey = grey || (rgb[0] == 128 && rgb[1] == 128 && rgb[2] == 128);
        colored = colored || (rgb[0] != 128 || rgb[1] != 128 || rgb[2] != 128);
    }
    Check(grey && colored, "valid and invalid have distinct colors");
    bool hasInvalidLegend = false;
    renderer->GetViewProps()->InitTraversal();
    while (auto *prop = renderer->GetViewProps()->GetNextProp())
        if (auto *legend = vtkLegendBoxActor::SafeDownCast(prop))
            hasInvalidLegend = legend->GetNumberOfEntries() == 1 &&
                               std::string(legend->GetEntryString(0)) == "Invalid / unmeasured";
    Check(hasInvalidLegend, "invalid color has an explicit legend entry");
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(400, 400);
    window->AddRenderer(renderer);
    const auto capture = [&](const char *path)
    {
        auto image = vtkSmartPointer<vtkWindowToImageFilter>::New();
        image->SetInput(window);
        image->ReadFrontBufferOff();
        image->Update();
        auto writer = vtkSmartPointer<vtkPNGWriter>::New();
        writer->SetFileName(path);
        writer->SetInputConnection(image->GetOutputPort());
        writer->Write();
    };
    renderer->ResetCamera();
    renderer->GetActiveCamera()->SetPosition(3.5, 3.5, 30);
    renderer->GetActiveCamera()->SetFocalPoint(3.5, 3.5, 2.5);
    renderer->GetActiveCamera()->SetViewUp(0, 1, 0);
    renderer->ResetCameraClippingRange();
    window->Render();
    capture("WallThickness-3D.png");
    for (std::size_t i = 0; i < candidate.field.samples->size(); ++i)
    {
        const auto &sample = (*candidate.field.samples)[i];
        if (sample.validity != ThicknessValidity::Valid || sample.source[2] < 3)
            continue;
        renderer->SetWorldPoint(sample.source[0], sample.source[1], sample.source[2], 1);
        renderer->WorldToDisplay();
        const auto *pixel = renderer->GetDisplayPoint();
        const auto picked =
            overlay->GetPickedSample(static_cast<int>(std::lround(pixel[0])),
                                     static_cast<int>(std::lround(pixel[1])), renderer);
        Check(picked && *picked == i, "3D pick maps to the exact source sample");
        break;
    }
    if (candidate.statistics.minimumSample)
        overlay->SetSelection(&(*candidate.field.samples)[*candidate.statistics.minimumSample]);
    overlay->SetOverlayState({{0, 0, 2.5}, {}});
    overlay->DetachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems() == 0, "detach removes owned props");
    auto slice = std::make_shared<ThicknessOverlay>(prepared, display, w.archive.input.unit,
                                                    HostRenderViewRole::TopDownSlice);
    slice->AttachRenderer(renderer);
    FeatureOverlayState state;
    state.cursor = {3, 3, 2.5};
    slice->SetOverlayState(state);
    renderer->ResetCamera();
    renderer->ResetCameraClippingRange();
    window->Render();
    capture("WallThickness-Slice.png");
    bool sliceMapped = false;
    auto *sliceActors = renderer->GetActors();
    sliceActors->InitTraversal();
    while (auto *actor = sliceActors->GetNextActor())
    {
        auto *mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        auto *cut = mapper ? mapper->GetInput() : nullptr;
        auto *ids =
            cut ? vtkIdTypeArray::SafeDownCast(cut->GetCellData()->GetArray("thickness.sample"))
                : nullptr;
        if (!ids || cut->GetNumberOfCells() == 0)
        {
            std::cerr << "slice cells=" << (cut ? cut->GetNumberOfCells() : -1)
                      << " arrays=" << (cut ? cut->GetCellData()->GetNumberOfArrays() : -1) << '\n';
            if (cut)
                for (int a = 0; a < cut->GetCellData()->GetNumberOfArrays(); ++a)
                    std::cerr << cut->GetCellData()->GetArrayName(a) << '\n';
            continue;
        }
        auto *cell = cut->GetCell(0);
        double center[3]{};
        for (vtkIdType p = 0; p < cell->GetNumberOfPoints(); ++p)
        {
            double point[3]{};
            cut->GetPoint(cell->GetPointId(p), point);
            for (int k = 0; k < 3; ++k)
                center[k] += point[k] / cell->GetNumberOfPoints();
        }
        renderer->SetWorldPoint(center[0], center[1], center[2], 1);
        renderer->WorldToDisplay();
        const auto *pixel = renderer->GetDisplayPoint();
        const auto picked =
            slice->GetPickedSample(static_cast<int>(std::lround(pixel[0])),
                                   static_cast<int>(std::lround(pixel[1])), renderer);
        sliceMapped = picked && *picked == static_cast<std::size_t>(ids->GetValue(0));
        if (!sliceMapped)
            std::cerr << "slice expected=" << ids->GetValue(0)
                      << " picked=" << (picked ? std::to_string(*picked) : "none")
                      << " center=" << center[0] << ',' << center[1] << ',' << center[2]
                      << " points=" << cell->GetNumberOfPoints() << '\n';
        break;
    }
    Check(sliceMapped, "actual cutter pick preserves exact sample mapping");
    slice->DetachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems() == 0, "slice cleanup");
}
} // namespace
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const std::string suite = argv[1];
    if (suite == "algorithm")
        Algorithm();
    else if (suite == "evaluation")
        Evaluation();
    else if (suite == "lifecycle")
        Lifecycle();
    else if (suite == "display")
        Display();
    else
        return 2;
    std::cout << "WallThickness " << suite << ": " << failures
              << " failures (synthetic fixtures).\n";
    return failures ? 1 : 0;
}
