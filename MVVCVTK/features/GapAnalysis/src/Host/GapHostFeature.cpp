#include "Host/GapHostFeature.h"

#include "Data/DataPayloads.h"
#include "Services/GapAnalysisService.h"

#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkIdList.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "GapAnalysis";
constexpr std::string_view gapResultBinding =
    "analysis.gaps.active";
const DataTypeId gapVoidTableType{
    "org.mvvcvtk.gap-analysis.void-table", 1 };
const DataTypeId gapStatisticsType{
    "org.mvvcvtk.gap-analysis.statistics", 1 };
const DataTypeId gapResultSetType{
    "org.mvvcvtk.gap-analysis.result-set", 1 };
const DataFacetId gapVoidRecordsFacet{ "tabular-gap-void-records" };
const DataFacetId gapStatisticsFacet{ "tabular-gap-statistics" };

std::shared_ptr<const LabelMap3DPayload> CreateLabelPayload(
    vtkImageData* labels,
    const GridGeometry3D& geometry)
{
    const auto voxelCount = GetGridVoxelCount(geometry);
    auto* scalars = labels && labels->GetPointData()
        ? labels->GetPointData()->GetScalars() : nullptr;
    if (!voxelCount || !scalars
        || scalars->GetNumberOfComponents() != 1
        || scalars->GetNumberOfTuples()
            != static_cast<vtkIdType>(*voxelCount)) {
        return {};
    }
    auto values = std::make_shared<std::vector<std::uint32_t>>(*voxelCount);
    for (std::size_t index = 0; index < *voxelCount; ++index) {
        const double value = scalars->GetComponent(
            static_cast<vtkIdType>(index), 0);
        if (!std::isfinite(value) || value < 0.0
            || value > std::numeric_limits<std::uint32_t>::max()
            || std::floor(value) != value) {
            return {};
        }
        (*values)[index] = static_cast<std::uint32_t>(value);
    }
    auto payload = std::make_shared<const LabelMap3DPayload>(
        geometry, std::move(values));
    return payload->GetValid() ? payload : nullptr;
}

std::shared_ptr<const SurfaceMeshPayload> CreateMeshPayload(
    vtkPolyData* mesh)
{
    if (!mesh) return {};
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(mesh);
    triangles->PassLinesOff();
    triangles->PassVertsOff();
    triangles->Update();
    auto* output = triangles->GetOutput();
    if (!output) return {};

    std::vector<double> vertices;
    if (auto* points = output->GetPoints()) {
        vertices.resize(
            static_cast<std::size_t>(points->GetNumberOfPoints()) * 3);
        for (vtkIdType index = 0;
            index < points->GetNumberOfPoints(); ++index) {
            points->GetPoint(
                index,
                vertices.data() + static_cast<std::size_t>(index) * 3);
        }
    }
    else if (output->GetNumberOfPoints() != 0) {
        return {};
    }

    std::vector<std::uint64_t> cells;
    if (auto* polys = output->GetPolys()) {
        auto ids = vtkSmartPointer<vtkIdList>::New();
        polys->InitTraversal();
        while (polys->GetNextCell(ids)) {
            if (ids->GetNumberOfIds() != 3) return {};
            for (vtkIdType index = 0; index < 3; ++index) {
                const auto value = ids->GetId(index);
                if (value < 0) return {};
                cells.push_back(static_cast<std::uint64_t>(value));
            }
        }
    }
    else if (output->GetNumberOfCells() != 0) {
        return {};
    }
    auto payload = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(cells));
    return payload->GetValid() ? payload : nullptr;
}

std::shared_ptr<const RecordTablePayload> CreateVoidTable(
    const std::vector<VoidRegion>& regions)
{
    std::vector<std::int64_t> ids;
    std::vector<std::int64_t> voxelCounts;
    std::vector<double> volumes;
    std::vector<double> equivalentDiameters;
    std::vector<double> radii;
    std::vector<double> diameters;
    std::vector<std::array<double, 3>> centers;
    std::vector<std::array<std::int64_t, 3>> centroids;
    std::vector<std::array<std::int64_t, 6>> boxes;
    std::vector<std::array<std::int64_t, 3>> seeds;
    std::vector<double> minimumGrayValues;
    std::vector<double> maximumGrayValues;
    std::vector<double> meanGrayValues;
    std::vector<double> grayStandardDeviations;
    std::vector<double> grayDeviations;
    std::vector<double> gaps;
    std::vector<double> compactnessValues;
    std::vector<double> surfaceAreas;
    std::vector<double> sphericityValues;
    std::vector<std::array<double, 3>> pcaDeviations;
    std::vector<double> pcaMaximumDeviationRatios;
    std::vector<double> pcaMinimumDeviationRatios;
    std::vector<double> projectedAreasX;
    std::vector<double> projectedAreasY;
    std::vector<double> projectedAreasZ;
    std::vector<std::array<double, 3>> projectedSizes;
    std::vector<double> probabilities;
    ids.reserve(regions.size());
    voxelCounts.reserve(regions.size());
    volumes.reserve(regions.size());
    equivalentDiameters.reserve(regions.size());
    radii.reserve(regions.size());
    diameters.reserve(regions.size());
    centers.reserve(regions.size());
    centroids.reserve(regions.size());
    boxes.reserve(regions.size());
    seeds.reserve(regions.size());
    minimumGrayValues.reserve(regions.size());
    maximumGrayValues.reserve(regions.size());
    meanGrayValues.reserve(regions.size());
    grayStandardDeviations.reserve(regions.size());
    grayDeviations.reserve(regions.size());
    gaps.reserve(regions.size());
    compactnessValues.reserve(regions.size());
    surfaceAreas.reserve(regions.size());
    sphericityValues.reserve(regions.size());
    pcaDeviations.reserve(regions.size());
    pcaMaximumDeviationRatios.reserve(regions.size());
    pcaMinimumDeviationRatios.reserve(regions.size());
    projectedAreasX.reserve(regions.size());
    projectedAreasY.reserve(regions.size());
    projectedAreasZ.reserve(regions.size());
    projectedSizes.reserve(regions.size());
    probabilities.reserve(regions.size());
    for (const auto& region : regions) {
        ids.push_back(region.id);
        voxelCounts.push_back(region.voxelCount);
        volumes.push_back(region.volumeMM3);
        equivalentDiameters.push_back(region.equivalentDiameterMM);
        radii.push_back(region.radiusMM);
        diameters.push_back(region.diameterMM);
        centers.push_back(region.centerMM);
        std::array<std::int64_t, 3> centroid{};
        std::transform(
            region.centroidMM.begin(), region.centroidMM.end(),
            centroid.begin(),
            [](const std::int32_t value) {
                return static_cast<std::int64_t>(value);
            });
        centroids.push_back(centroid);
        std::array<std::int64_t, 6> box{};
        std::transform(
            region.bbox.begin(), region.bbox.end(), box.begin(),
            [](const std::int32_t value) {
                return static_cast<std::int64_t>(value);
            });
        boxes.push_back(box);
        std::array<std::int64_t, 3> seed{};
        std::transform(
            region.seedVoxel.begin(), region.seedVoxel.end(), seed.begin(),
            [](const std::int32_t value) {
                return static_cast<std::int64_t>(value);
            });
        seeds.push_back(seed);
        minimumGrayValues.push_back(region.minGray);
        maximumGrayValues.push_back(region.maxGray);
        meanGrayValues.push_back(region.meanGray);
        grayStandardDeviations.push_back(region.stdDevGray);
        grayDeviations.push_back(region.grayDeviation);
        gaps.push_back(region.gapMM);
        compactnessValues.push_back(region.compactness);
        surfaceAreas.push_back(region.surfaceAreaMM2);
        sphericityValues.push_back(region.sphericity);
        pcaDeviations.push_back({
            static_cast<double>(region.pcaDeviation[0]),
            static_cast<double>(region.pcaDeviation[1]),
            static_cast<double>(region.pcaDeviation[2]) });
        pcaMaximumDeviationRatios.push_back(
            region.pcaMaxDeviationRatio);
        pcaMinimumDeviationRatios.push_back(
            region.pcaMinDeviationRatio);
        projectedAreasX.push_back(region.projectedAreaXMM2);
        projectedAreasY.push_back(region.projectedAreaYMM2);
        projectedAreasZ.push_back(region.projectedAreaZMM2);
        projectedSizes.push_back({
            static_cast<double>(region.projectedSize[0]),
            static_cast<double>(region.projectedSize[1]),
            static_cast<double>(region.projectedSize[2]) });
        probabilities.push_back(region.defectProbability);
    }
    auto table = std::make_shared<const RecordTablePayload>(
        gapVoidTableType,
        "gap-analysis.void-regions",
        std::vector<RecordColumn>{
            { "void-id", std::move(ids) },
            { "voxel-count", std::move(voxelCounts) },
            { "volume-mm3", std::move(volumes) },
            { "equivalent-diameter-mm", std::move(equivalentDiameters) },
            { "radius-mm", std::move(radii) },
            { "diameter-mm", std::move(diameters) },
            { "center-mm", std::move(centers) },
            { "centroid-mm", std::move(centroids) },
            { "voxel-bbox", std::move(boxes) },
            { "seed-voxel", std::move(seeds) },
            { "gray-min", std::move(minimumGrayValues) },
            { "gray-max", std::move(maximumGrayValues) },
            { "gray-mean", std::move(meanGrayValues) },
            { "gray-standard-deviation",
              std::move(grayStandardDeviations) },
            { "gray-deviation", std::move(grayDeviations) },
            { "gap-mm", std::move(gaps) },
            { "compactness", std::move(compactnessValues) },
            { "surface-area-mm2", std::move(surfaceAreas) },
            { "sphericity", std::move(sphericityValues) },
            { "pca-deviation", std::move(pcaDeviations) },
            { "pca-maximum-deviation-ratio",
              std::move(pcaMaximumDeviationRatios) },
            { "pca-minimum-deviation-ratio",
              std::move(pcaMinimumDeviationRatios) },
            { "projected-area-x-mm2", std::move(projectedAreasX) },
            { "projected-area-y-mm2", std::move(projectedAreasY) },
            { "projected-area-z-mm2", std::move(projectedAreasZ) },
            { "projected-size-voxel", std::move(projectedSizes) },
            { "defect-probability", std::move(probabilities) } });
    return table->GetValid() ? table : nullptr;
}

std::shared_ptr<const RecordTablePayload> CreateStatisticsTable(
    const GapStatistics& statistics)
{
    auto table = std::make_shared<const RecordTablePayload>(
        gapStatisticsType,
        "gap-analysis.statistics",
        std::vector<RecordColumn>{
            { "object-voxel-count",
              std::vector<std::uint64_t>{ statistics.objectVoxelCount } },
            { "void-voxel-count",
              std::vector<std::uint64_t>{ statistics.voidVoxelCount } },
            { "object-volume-mm3",
              std::vector<double>{ statistics.objectVolumeMM3 } },
            { "void-volume-mm3",
              std::vector<double>{ statistics.voidVolumeMM3 } },
            { "porosity-ratio",
              std::vector<double>{ statistics.porosityRatio } } });
    return table->GetValid() ? table : nullptr;
}

GapHostResult BuildHostResult(
    const GapHostState& state,
    const GapResultStatus status,
    std::string message)
{
    GapHostResult result;
    result.status = status;
    result.commitId = state.commitId;
    result.sourceRevision = state.sourceRevision;
    result.labelMap = state.labelMap;
    result.voidTable = state.voidTable;
    result.voidMesh = state.voidMesh;
    result.statisticsData = state.statisticsData;
    result.resultSet = state.resultSet;
    result.statistics = state.statistics;
    result.message = std::move(message);
    return result;
}

} // namespace

class GapHostFeature::Impl final {
public:
    struct CompleteItem final {
        std::mutex mutex;
        GapHostCallback onComplete;
        bool isActive = true;
        bool isSent = false;
    };

    struct ViewCandidate final {
        GapViewRequest request;
        std::vector<std::string> activeViewIds;
        VtkImageGridSnapshot source;
        DataBinding resultBinding;
    };

    explicit Impl(GapHostConfig config)
        : m_config(std::move(config))
        , m_service(std::make_unique<GapAnalysisService>())
    {
    }

    bool AttachHost(
        GapHostFeature& owner,
        const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    bool SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete);
    GapHostState GetState() const;

    static constexpr std::string_view FeatureId =
        "GapAnalysis";

private:
    static bool GetImageReady(vtkImageData* image);
    static bool GetCharMatched(
        const InteractionEvent& event,
        char keyCode);
    static bool GetChordMatched(
        const InteractionEvent& event,
        const HostKeyChord& chord);
    static bool GetChordValid(const HostKeyChord& chord);
    static bool GetStartValid(
        const GapHostStartParams& params);
    static bool GetSnapshotValid(
        const VtkImageGridSnapshot& snapshot);
    static std::optional<Orientation> GetSliceOrient(
        HostRenderViewRole role);
    static bool SendComplete(
        const std::shared_ptr<CompleteItem>& item,
        GapHostResult result);

    std::optional<ViewCandidate> GetViewCandidate(
        const GapHostStartParams& start) const;
    bool StartView(
        const GapHostStartParams& start,
        GapHostCallback onComplete);
    bool SwitchOverlay();
    bool ExitView();
    bool SetDataTypes();
    bool GetSourceSame() const;
    bool SetCompletedResult(const GapAnalysisResult& candidate);
    bool ClearResultBinding();
    void SetFailedResult(GapResultStatus status, std::string message);
    InteractionResult OnInput(
        const InteractionEvent& event);
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool ClearComplete();
    bool ClearBorrowed();
    bool GetOwnerThread() const;

    GapHostConfig m_config;
    std::unique_ptr<GapAnalysisService> m_service;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<CompleteItem> m_completeItem;
    VtkImageGridSnapshot m_requestSource;
    DataBinding m_requestResultBinding;
    GapHostState m_state;
    std::thread::id m_ownerThread;
    bool m_isSwitchDown = false;
    bool m_isExitDown = false;
    bool m_isExitPending = false;
    bool m_isRequestPending = false;
    bool m_isInputAttached = false;
    bool m_isAttached = false;
};

bool GapHostFeature::Impl::GetImageReady(
    vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = {};
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

bool GapHostFeature::Impl::GetCharMatched(
    const InteractionEvent& event,
    const char keyCode)
{
    if (keyCode == 0) {
        return false;
    }
    const char upper = keyCode >= 'a' && keyCode <= 'z'
        ? static_cast<char>(keyCode - 'a' + 'A')
        : keyCode;
    return event.keyCode == keyCode
        || event.keyCode == upper
        || event.keySym == std::string(1, keyCode)
        || event.keySym == std::string(1, upper);
}

bool GapHostFeature::Impl::GetChordMatched(
    const InteractionEvent& event,
    const HostKeyChord& chord)
{
    const bool hasKey = GetCharMatched(
        event, chord.keyCode)
        || (!chord.keySym.empty()
            && event.keySym == chord.keySym);
    return hasKey
        && event.isCtrlDown == chord.isCtrlDown
        && event.isAltDown == chord.isAltDown
        && event.isShiftDown == chord.isShiftDown;
}

bool GapHostFeature::Impl::GetChordValid(
    const HostKeyChord& chord)
{
    return chord.keyCode != 0 || !chord.keySym.empty();
}

bool GapHostFeature::Impl::GetStartValid(
    const GapHostStartParams& params)
{
    if (params.targetViews.viewIds.empty()
        && params.targetViews.viewRoles.empty()) {
        return false;
    }
    if (!std::isfinite(params.surface.dataRangeRatio)
        || !std::isfinite(params.surface.absoluteIsoValue)
        || !std::isfinite(params.surface.backgroundMean)
        || !std::isfinite(params.surface.materialMean)
        || !std::isfinite(params.voidParams.minVolumeMM3)) {
        return false;
    }

    switch (params.surface.isoMode) {
    case GapIsoMode::DataRangeRatio:
        if (params.surface.dataRangeRatio < 0.0
            || params.surface.dataRangeRatio > 1.0) {
            return false;
        }
        break;
    case GapIsoMode::AbsoluteValue:
        break;
    default:
        return false;
    }

    return params.surface.backgroundMean
            <= params.surface.materialMean
        && params.voidParams.minVolumeMM3 >= 0.0
        && params.voidParams.minVolumeMM3
            <= static_cast<double>(
                (std::numeric_limits<float>::max)())
        && (params.voidParams.minVolumeMM3 == 0.0
            || params.voidParams.minVolumeMM3
                >= static_cast<double>(
                    (std::numeric_limits<float>::denorm_min)()));
}

bool GapHostFeature::Impl::GetSnapshotValid(
    const VtkImageGridSnapshot& snapshot)
{
    const auto* payload = snapshot && snapshot->data
        ? dynamic_cast<const ImageGrid3DPayload*>(
            snapshot->data->payload.get())
        : nullptr;
    if (!snapshot || !snapshot->data || !snapshot->binding
        || !snapshot->binding->target
        || *snapshot->binding->target != snapshot->data->self
        || !payload || !payload->GetValid()
        || !GetImageReady(snapshot->image)
        || payload->GetComponentCount() != 1) {
        return false;
    }

    const auto& geometry = payload->GetGeometry();
    int dimensions[3] = {};
    snapshot->image->GetDimensions(dimensions);
    for (int axis = 0; axis < 3; ++axis) {
        if (dimensions[axis] != geometry.dimensions[axis]) {
            return false;
        }
    }
    return true;
}

std::optional<Orientation>
GapHostFeature::Impl::GetSliceOrient(
    const HostRenderViewRole role)
{
    switch (role) {
    case HostRenderViewRole::TopDownSlice:
        return Orientation::Top_down;
    case HostRenderViewRole::FrontBackSlice:
        return Orientation::Front_back;
    case HostRenderViewRole::LeftRightSlice:
        return Orientation::Left_right;
    default:
        return std::nullopt;
    }
}

bool GapHostFeature::Impl::SendComplete(
    const std::shared_ptr<CompleteItem>& item,
    GapHostResult result)
{
    if (!item) {
        return false;
    }

    GapHostCallback callback;
    {
        const std::lock_guard<std::mutex> lock(item->mutex);
        if (!item->isActive || item->isSent) {
            return false;
        }
        item->isSent = true;
        callback = std::move(item->onComplete);
    }
    if (callback) {
        try {
            callback(std::move(result));
        }
        catch (...) {
        }
    }
    return true;
}

std::optional<GapHostFeature::Impl::ViewCandidate>
GapHostFeature::Impl::GetViewCandidate(
    const GapHostStartParams& start) const
{
    if (!m_views
        || !m_data
        || !GetStartValid(start)) {
        return std::nullopt;
    }

    const auto snapshot = m_data->GetPrimaryImage();
    if (!GetSnapshotValid(snapshot)) {
        return std::nullopt;
    }
    const auto views = m_views->GetViews(start.targetViews);
    if (views.empty()) {
        return std::nullopt;
    }

    ViewCandidate candidate;
    candidate.source = snapshot;
    candidate.request.graphInput = snapshot;
    candidate.resultBinding.name = std::string(gapResultBinding);
    if (const auto binding = m_data->GetDataBinding(
            snapshot->graph, gapResultBinding)) {
        candidate.resultBinding = *binding;
    }
    candidate.request.surface = start.surface;
    candidate.request.voidParams = start.voidParams;

    for (const auto& view : views) {
        const auto port = m_views->GetOverlayPort(view.id);
        if (view.id.empty() || !port) {
            continue;
        }
        candidate.activeViewIds.push_back(view.id);
        if (view.role
                == HostRenderViewRole::Primary3D
            || view.role
                == HostRenderViewRole::Composite3D) {
            candidate.request.meshTargets.push_back(
                port);
            continue;
        }
        if (const auto orientation =
                GetSliceOrient(view.role)) {
            candidate.request.sliceTargets.push_back(
                { *orientation, port });
        }
    }

    if (candidate.request.meshTargets.empty()
        && candidate.request.sliceTargets.empty()) {
        return std::nullopt;
    }
    return candidate;
}

bool GapHostFeature::Impl::AttachHost(
    GapHostFeature& owner,
    const HostFeatureContext& context)
{
    if (m_isAttached) {
        return false;
    }
    if (!context.views
        || !context.data
        || !context.host
        || (m_config.inputViews.viewIds.empty()
            && m_config.inputViews.viewRoles.empty())
        || !GetChordValid(m_config.keys.switchOverlay)
        || !GetChordValid(m_config.keys.exit)
        || !GetStartValid(m_config.defaultStart)) {
        return false;
    }

    const auto weakOwner = owner.weak_from_this();
    if (weakOwner.expired()) {
        return false;
    }

    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    if (!SetDataTypes()) {
        ClearBorrowed();
        return false;
    }

    HostInputBinding binding;
    binding.featureId = std::string(FeatureId);
    binding.targetViews = m_config.inputViews;
    binding.onInput = [weakOwner](
        const InteractionEvent& event) {
        const auto feature = weakOwner.lock();
        return feature && feature->m_impl
            ? feature->m_impl->OnInput(event)
            : InteractionResult{};
    };
    if (!m_host->AttachInput(std::move(binding))) {
        ClearBorrowed();
        return false;
    }

    m_isInputAttached = true;
    m_isAttached = true;
    m_isSwitchDown = false;
    m_isExitDown = false;
    m_isExitPending = false;
    m_isRequestPending = false;
    m_state = {};
    return true;
}

bool GapHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) {
        return true;
    }
    if (!GetOwnerThread()) {
        return false;
    }

    bool isInputDetached = !m_isInputAttached;
    if (m_isInputAttached && m_host) {
        try {
            isInputDetached =
                m_host->DetachInput(FeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
        if (isInputDetached) {
            m_isInputAttached = false;
        }
    }

    // 即使输入端口暂时拒绝移除，也必须先完成强清理；失败重试只保留 Host 控制能力和 owner thread。
    ClearComplete();
    if (m_service) {
        m_service->ClearView();
    }
    (void)SetActiveViews({});
    (void)ClearResultBinding();
    m_requestSource.reset();
    m_requestResultBinding = {};
    m_state = {};
    m_isRequestPending = false;
    m_views.reset();
    m_data.reset();
    m_isSwitchDown = false;
    m_isExitDown = false;
    m_isExitPending = false;

    if (!isInputDetached) {
        return false;
    }

    m_isAttached = false;
    ClearBorrowed();
    return true;
}

bool GapHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return false;
    }

    if (m_service->GetDisplayTickNeeded()) {
        m_service->OnDisplayTick(nullptr);
    }

    if (m_isRequestPending) {
        const auto analysisState = m_service->GetAnalysisState();
        if (analysisState == GapAnalysisState::Succeeded) {
            GapAnalysisResult candidate;
            if (m_service->GetCompletedResult(candidate)) {
                (void)SetCompletedResult(candidate);
            }
        }
        else if (analysisState == GapAnalysisState::Failed
            || analysisState == GapAnalysisState::Idle) {
            SetFailedResult(
                GapResultStatus::Failed,
                "Gap analysis failed before data publication.");
        }
    }

    if (!m_isRequestPending
        && !m_isExitPending
        && GetDataRevisionRefValid(m_state.resultSet)
        && m_data) {
        const auto graph = m_data->GetDataGraph();
        if (m_data->GetDataRelation(
                graph,
                m_state.resultSet,
                "source-volume",
                primaryVolumeBinding)
            == DataRelationStatus::OutOfDateRelativeToCurrentBinding) {
            (void)ClearResultBinding();
            m_state.analysisState = GapAnalysisState::Stale;
            ClearComplete();
            if (m_service->ExitView()) {
                m_isExitPending = true;
            }
        }
    }

    if (m_isExitPending
        && !m_service->GetDisplayTickNeeded()) {
        if (SetActiveViews({})) {
            m_isExitPending = false;
            ClearComplete();
        }
    }
    return true;
}

bool GapHostFeature::Impl::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return false;
    }

    switch (request.action) {
    case GapHostAction::Start:
        if (!request.start) {
            return false;
        }
        return StartView(
            *request.start,
            std::move(onComplete));
    case GapHostAction::Overlay:
        if (onComplete) {
            return false;
        }
        return SwitchOverlay();
    case GapHostAction::Exit:
        if (onComplete) {
            return false;
        }
        return ExitView();
    case GapHostAction::None:
        return false;
    }
    return false;
}

GapHostState GapHostFeature::Impl::GetState() const
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return {};
    }

    auto state = m_state;
    state.isViewActive = m_service->GetViewOn();
    state.isExitPending = m_isExitPending;
    return state;
}

bool GapHostFeature::Impl::StartView(
    const GapHostStartParams& start,
    GapHostCallback onComplete)
{
    auto candidate = GetViewCandidate(start);
    if (!candidate) {
        return false;
    }

    auto completeItem =
        std::make_shared<CompleteItem>();
    completeItem->onComplete = std::move(onComplete);
    const bool isAccepted = m_service->StartView(
        std::move(candidate->request));
    if (!isAccepted) {
        return false;
    }
    if (!SetActiveViews(candidate->activeViewIds)) {
        m_service->ClearView();
        ClearComplete();
        m_requestSource.reset();
        m_requestResultBinding = {};
        m_isRequestPending = false;
        m_isExitPending = false;
        return false;
    }

    ClearComplete();
    m_completeItem = std::move(completeItem);
    m_requestSource = std::move(candidate->source);
    m_requestResultBinding = std::move(candidate->resultBinding);
    m_state = {};
    m_state.analysisState = GapAnalysisState::Running;
    m_state.sourceRevision = m_requestSource->data->self;
    m_isRequestPending = true;
    m_isExitPending = false;
    return true;
}

bool GapHostFeature::Impl::SwitchOverlay()
{
    return !m_isExitPending
        && m_service
        && m_service->GetViewOn()
        && m_service->SwitchOverlay();
}

bool GapHostFeature::Impl::ExitView()
{
    if (m_isExitPending
        || !m_service
        || !m_service->GetViewOn()) {
        return false;
    }
    if (!ClearResultBinding()) return false;
    const bool isExited = m_service->ExitView();
    if (!isExited) return false;
    if (m_isRequestPending) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap analysis was cancelled before publication.");
    }
    ClearComplete();
    m_isExitPending = true;
    return true;
}

bool GapHostFeature::Impl::SetDataTypes()
{
    if (!m_data) return false;
    const auto setType = [this](
        const DataTypeId& type,
        DataTypeDescriptor descriptor) {
        const auto graph = m_data->GetDataGraph();
        return (graph.view
                && !graph.view->GetDataFacets(type).empty())
            || m_data->SetDataType(std::move(descriptor));
    };
    return setType(
            gapVoidTableType,
            GetRecordTableDescriptor(
                gapVoidTableType,
                { DataFacets::tabularRecords, gapVoidRecordsFacet }))
        && setType(
            gapStatisticsType,
            GetRecordTableDescriptor(
                gapStatisticsType,
                { DataFacets::tabularRecords, gapStatisticsFacet }))
        && setType(
            gapResultSetType,
            GetDataCollectionDescriptor(gapResultSetType));
}

bool GapHostFeature::Impl::GetSourceSame() const
{
    if (!m_data || !m_requestSource || !m_requestSource->data
        || !m_requestSource->binding) {
        return false;
    }
    const auto current = m_data->GetPrimaryImage();
    return current && current->data && current->binding
        && current->data->self == m_requestSource->data->self
        && current->binding->revision
            == m_requestSource->binding->revision;
}

bool GapHostFeature::Impl::SetCompletedResult(
    const GapAnalysisResult& candidate)
{
    if (!m_isRequestPending || !m_data || !m_requestSource
        || !m_requestSource->data || !m_requestSource->binding) {
        return false;
    }
    const auto* sourcePayload = dynamic_cast<const ImageGrid3DPayload*>(
        m_requestSource->data->payload.get());
    if (!candidate.isSucceeded || !sourcePayload
        || !sourcePayload->GetValid()) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result candidate is invalid.");
        return false;
    }

    auto labels = CreateLabelPayload(
        candidate.labelImage, sourcePayload->GetGeometry());
    auto mesh = CreateMeshPayload(candidate.voidMesh);
    auto voids = CreateVoidTable(candidate.voids);
    auto statistics = CreateStatisticsTable(candidate.statistics);
    if (!labels || !mesh || !voids || !statistics) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result payload construction failed.");
        return false;
    }

    const auto labelEntity = m_data->CreateDataEntityId();
    const auto voidEntity = m_data->CreateDataEntityId();
    const auto meshEntity = m_data->CreateDataEntityId();
    const auto statisticsEntity = m_data->CreateDataEntityId();
    const auto resultEntity = m_data->CreateDataEntityId();
    const DataRevisionRef labelRef{ labelEntity, 1 };
    const DataRevisionRef voidRef{ voidEntity, 1 };
    const DataRevisionRef meshRef{ meshEntity, 1 };
    const DataRevisionRef statisticsRef{ statisticsEntity, 1 };
    const DataRevisionRef resultRef{ resultEntity, 1 };
    auto resultSet = std::make_shared<const DataCollectionPayload>(
        gapResultSetType,
        std::vector<DataCollectionEntry>{
            { "labels", labelRef },
            { "void-regions", voidRef },
            { "void-surface", meshRef },
            { "statistics", statisticsRef } });
    if (!resultSet->GetValid()) {
        SetFailedResult(
            GapResultStatus::Failed,
            "Gap result-set construction failed.");
        return false;
    }

    const auto sourceRef = m_requestSource->data->self;
    DataExpectation sourceExpectation;
    sourceExpectation.kind = DataExpectationKind::Binding;
    sourceExpectation.binding = std::string(primaryVolumeBinding);
    sourceExpectation.expectedBindingRevision =
        m_requestSource->binding->revision;
    sourceExpectation.isTargetChecked = true;
    sourceExpectation.expectedTarget = sourceRef;

    DataTransaction transaction;
    transaction.expectations.push_back(std::move(sourceExpectation));
    transaction.outputs = {
        DataRevisionDraft{
            labelEntity, 0, DataTypes::labelMap3D,
            { DataInputRef{ "source-volume", sourceRef } },
            std::move(labels),
            DataProvenance{
                std::string(featureId), "analyze-labels", "1", "{}" } },
        DataRevisionDraft{
            voidEntity, 0, gapVoidTableType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef } },
            std::move(voids),
            DataProvenance{
                std::string(featureId), "project-void-regions", "1", "{}" } },
        DataRevisionDraft{
            meshEntity, 0, DataTypes::surfaceMesh,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef } },
            std::move(mesh),
            DataProvenance{
                std::string(featureId), "extract-void-surface", "1", "{}" } },
        DataRevisionDraft{
            statisticsEntity, 0, gapStatisticsType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef },
              DataInputRef{ "void-regions", voidRef } },
            std::move(statistics),
            DataProvenance{
                std::string(featureId), "project-statistics", "1", "{}" } },
        DataRevisionDraft{
            resultEntity, 0, gapResultSetType,
            { DataInputRef{ "source-volume", sourceRef },
              DataInputRef{ "labels", labelRef },
              DataInputRef{ "void-regions", voidRef },
              DataInputRef{ "void-surface", meshRef },
              DataInputRef{ "statistics", statisticsRef } },
            std::move(resultSet),
            DataProvenance{
                std::string(featureId), "collect-results", "1", "{}" } }
    };
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(gapResultBinding),
        m_requestResultBinding.revision,
        true,
        m_requestResultBinding.target,
        resultRef });

    const auto commit = m_data->SetDataCommit(std::move(transaction));
    if (commit.status != DataCommitStatus::Succeeded) {
        SetFailedResult(
            GetSourceSame()
                ? GapResultStatus::Failed
                : GapResultStatus::SourceChanged,
            GetSourceSame()
                ? "Gap data transaction was rejected."
                : "Gap source changed before publication.");
        return false;
    }

    GapHostState state;
    state.analysisState = GapAnalysisState::Succeeded;
    state.statistics = candidate.statistics;
    state.commitId = commit.commitId;
    state.sourceRevision = sourceRef;
    state.labelMap = labelRef;
    state.voidTable = voidRef;
    state.voidMesh = meshRef;
    state.statisticsData = statisticsRef;
    state.resultSet = resultRef;
    m_state = state;

    const auto graph = m_data->GetDataGraph();
    auto labelView = m_data->GetLabelMap(graph, labelRef);
    auto meshView = m_data->GetSurfaceMesh(graph, meshRef);
    const bool isDisplayed = m_service->SetCommittedView(
        std::move(labelView), std::move(meshView));
    const auto callback = m_completeItem;
    m_isRequestPending = false;
    m_requestSource.reset();
    m_requestResultBinding = {};
    (void)SendComplete(
        callback,
        BuildHostResult(
            m_state,
            isDisplayed
                ? GapResultStatus::Succeeded
                : GapResultStatus::SucceededWithDisplayFailure,
            isDisplayed
                ? "Gap data committed and displayed."
                : "Gap data committed; display attach failed."));
    ClearComplete();
    return true;
}

bool GapHostFeature::Impl::ClearResultBinding()
{
    if (!m_data) return false;
    const auto graph = m_data->GetDataGraph();
    const auto binding = m_data->GetDataBinding(graph, gapResultBinding);
    if (!binding || !binding->target) return true;
    const bool ownsState = GetDataRevisionRefValid(m_state.resultSet)
        && *binding->target == m_state.resultSet;
    const bool ownsRequest = m_requestResultBinding.target
        && *binding->target == *m_requestResultBinding.target;
    if (!ownsState && !ownsRequest) return true;
    DataTransaction transaction;
    transaction.bindings.push_back(DataBindingUpdate{
        std::string(gapResultBinding),
        binding->revision,
        true,
        binding->target,
        {} });
    return m_data->SetDataCommit(std::move(transaction)).status
        == DataCommitStatus::Succeeded;
}

void GapHostFeature::Impl::SetFailedResult(
    const GapResultStatus status,
    std::string message)
{
    if (!m_isRequestPending) return;
    m_state.analysisState = status == GapResultStatus::SourceChanged
        ? GapAnalysisState::Stale
        : GapAnalysisState::Failed;
    const auto callback = m_completeItem;
    m_isRequestPending = false;
    m_service->SetViewCommitFailed();
    if (status == GapResultStatus::SourceChanged
        && m_service->GetViewOn()
        && m_service->ExitView()) {
        m_isExitPending = true;
    }
    (void)SendComplete(
        callback,
        BuildHostResult(m_state, status, std::move(message)));
    ClearComplete();
    m_requestSource.reset();
    m_requestResultBinding = {};
}

InteractionResult GapHostFeature::Impl::OnInput(
    const InteractionEvent& event)
{
    const bool isSwitch = GetChordMatched(
        event, m_config.keys.switchOverlay);
    const bool isExit = GetChordMatched(
        event, m_config.keys.exit);
    if (!isSwitch && !isExit) {
        return {};
    }

    if (event.eventKind
        == InteractionEventKind::KeyRelease) {
        const bool wasDown =
            (isSwitch && m_isSwitchDown)
            || (isExit && m_isExitDown);
        if (isSwitch) {
            m_isSwitchDown = false;
        }
        if (isExit) {
            m_isExitDown = false;
        }
        return wasDown
            ? InteractionResult{ true, true }
            : InteractionResult{};
    }
    if (event.eventKind
        == InteractionEventKind::TextInput) {
        return (isSwitch && m_isSwitchDown)
                || (isExit && m_isExitDown)
            ? InteractionResult{ true, true }
            : InteractionResult{};
    }
    if (event.eventKind
        != InteractionEventKind::KeyPress) {
        return {};
    }

    if (isExit) {
        if (!m_service->GetViewOn()
            || m_isExitPending) {
            return {};
        }
        if (m_isExitDown) {
            return { true, true };
        }
        m_isExitDown = true;
        GapHostRequest request;
        request.action = GapHostAction::Exit;
        (void)SendRequest(std::move(request), nullptr);
        return { true, true };
    }

    if (m_isSwitchDown) {
        return { true, true };
    }
    m_isSwitchDown = true;
    GapHostRequest request;
    if (m_service->GetViewOn()
        && !m_isExitPending) {
        request.action = GapHostAction::Overlay;
    }
    else {
        request.action = GapHostAction::Start;
        request.start = m_config.defaultStart;
    }
    (void)SendRequest(std::move(request), nullptr);
    return { true, true };
}

bool GapHostFeature::Impl::SetActiveViews(
    const std::vector<std::string>& viewIds) const
{
    return m_host
        && m_host->SetActiveViews(viewIds);
}

bool GapHostFeature::Impl::ClearComplete()
{
    if (!m_completeItem) {
        return true;
    }
    {
        const std::lock_guard<std::mutex> lock(
            m_completeItem->mutex);
        m_completeItem->isActive = false;
        m_completeItem->onComplete = nullptr;
    }
    m_completeItem.reset();
    return true;
}

bool GapHostFeature::Impl::ClearBorrowed()
{
    if (m_host) {
        (void)m_host->SetActiveViews({});
    }
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    m_isInputAttached = false;
    m_requestSource.reset();
    m_requestResultBinding = {};
    m_isRequestPending = false;
    ClearComplete();
    return true;
}

bool GapHostFeature::Impl::GetOwnerThread() const
{
    return m_ownerThread == std::this_thread::get_id();
}

GapHostFeature::GapHostFeature(GapHostConfig config)
    : m_impl(std::make_unique<Impl>(
        std::move(config)))
{
}

GapHostFeature::~GapHostFeature() noexcept = default;

std::string_view GapHostFeature::GetFeatureId() const noexcept
{
    return Impl::FeatureId;
}

FeatureDataContract GapHostFeature::GetDataContract() const
{
    return FeatureDataContract{
        { DataInputSpec{
            "source-volume", DataFacets::scalarGrid3D, true } },
        {
            DataOutputSpec{
                "labels", DataTypes::labelMap3D,
                { DataFacets::labelMap3D } },
            DataOutputSpec{
                "void-regions", gapVoidTableType,
                { DataFacets::tabularRecords, gapVoidRecordsFacet } },
            DataOutputSpec{
                "void-surface", DataTypes::surfaceMesh,
                { DataFacets::surfaceMesh } },
            DataOutputSpec{
                "statistics", gapStatisticsType,
                { DataFacets::tabularRecords, gapStatisticsFacet } },
            DataOutputSpec{
                "result-set", gapResultSetType,
                { DataFacets::dataCollection } }
        } };
}

bool GapHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl
        && m_impl->AttachHost(*this, context);
}

bool GapHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool GapHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

bool GapHostFeature::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request),
            std::move(onComplete));
}

GapHostState GapHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : GapHostState{};
}
