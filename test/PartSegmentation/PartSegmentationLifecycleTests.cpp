#include "PartSegmentationTestCases.h"
#include "../TestDataPort.h"

#include "App/Services/FeatureViewService.h"
#include "Host/PartSegmentationHostFeature.h"
#include "Render/Contracts/OverlayService.h"
#include "Services/PartSegmentationService.h"

#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkSOADataArrayTemplate.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

class OverlayStub final : public OverlayService {
public:
    bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) override
    {
        if (!overlay || m_failNextAttach) {
            m_failNextAttach = false;
            return false;
        }
        m_overlays.push_back(std::move(overlay));
        ++m_attachCount;
        return true;
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        const auto found = std::find(
            m_overlays.begin(), m_overlays.end(), overlay);
        if (found == m_overlays.end()) return;
        m_overlays.erase(found);
        ++m_removeCount;
    }

    void ClearOverlays() noexcept override
    {
        m_removeCount += static_cast<int>(m_overlays.size());
        m_overlays.clear();
    }

    void SetFailNextAttach() noexcept { m_failNextAttach = true; }
    std::size_t GetOverlayCount() const noexcept
    {
        return m_overlays.size();
    }
    int GetAttachCount() const noexcept { return m_attachCount; }
    int GetRemoveCount() const noexcept { return m_removeCount; }
    const void* GetLabelPointer() const
    {
        if (m_overlays.empty()) return nullptr;
        vtkNew<vtkRenderer> renderer;
        const auto& overlay = m_overlays.back();
        overlay->AttachRenderer(renderer);
        auto* props = renderer->GetViewProps();
        props->InitTraversal();
        auto* slice = vtkImageSlice::SafeDownCast(props->GetNextProp());
        auto* mapper = slice
            ? vtkImageResliceMapper::SafeDownCast(slice->GetMapper())
            : nullptr;
        auto* image = mapper
            ? vtkImageData::SafeDownCast(mapper->GetInput())
            : nullptr;
        const void* pointer = image ? image->GetScalarPointer() : nullptr;
        overlay->DetachRenderer(renderer);
        return pointer;
    }

private:
    std::vector<std::shared_ptr<FeatureOverlay>> m_overlays;
    int m_attachCount = 0;
    int m_removeCount = 0;
    bool m_failNextAttach = false;
};

class FeatureViewStub final : public FeatureViewService {
public:
    bool SetInteracting(const InteractionSource&, bool) override
    {
        return true;
    }

    std::optional<std::array<double, 16>> GetModelToWorld() const override
    {
        return std::nullopt;
    }

    std::optional<std::array<double, 3>> GetWorldPosition(
        const std::array<double, 3>&) const override
    {
        return std::nullopt;
    }

    std::optional<RenderInputStamp> GetRenderInputStamp() const override
    {
        return m_stamp;
    }

    bool AttachRenderEffect(std::shared_ptr<RenderEffect>) override
    {
        return true;
    }

    bool DetachRenderEffect(const RenderEffect*) override
    {
        return true;
    }

    bool SetRenderNeeded() override { return true; }

    void SetInputStamp(const RenderInputStamp stamp) noexcept
    {
        m_stamp = stamp;
    }

private:
    std::optional<RenderInputStamp> m_stamp;
};

class ViewDirectoryStub final : public FeatureViewDirectory {
public:
    ViewDirectoryStub()
        : m_views{
            { "part-primary", HostRenderViewRole::Primary3D },
            { "part-top", HostRenderViewRole::TopDownSlice },
            { "part-front", HostRenderViewRole::FrontBackSlice },
            { "part-left", HostRenderViewRole::LeftRightSlice }
        }
    {
        for (const auto& view : m_views) {
            m_overlays.emplace(
                view.id, std::make_shared<OverlayStub>());
            m_featureViews.emplace(
                view.id, std::make_shared<FeatureViewStub>());
        }
    }

    std::vector<HostFeatureView> GetViews(
        const HostViewTargets& targets) const override
    {
        std::vector<HostFeatureView> selected;
        for (const auto& view : m_views) {
            const bool hasId = std::find(
                targets.viewIds.begin(), targets.viewIds.end(), view.id)
                != targets.viewIds.end();
            const bool hasRole = std::find(
                targets.viewRoles.begin(), targets.viewRoles.end(), view.role)
                != targets.viewRoles.end();
            if (hasId || hasRole) selected.push_back(view);
        }
        return selected;
    }

    std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string& viewId) const override
    {
        const auto found = m_featureViews.find(viewId);
        return found != m_featureViews.end() ? found->second : nullptr;
    }

    std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string& viewId) const override
    {
        return GetOverlay(viewId);
    }

    std::optional<HostInputView> GetInputView(
        const HostViewTarget&) const override
    {
        return std::nullopt;
    }

    std::shared_ptr<OverlayStub> GetOverlay(
        const std::string& viewId) const
    {
        const auto found = m_overlays.find(viewId);
        return found != m_overlays.end() ? found->second : nullptr;
    }

    std::size_t GetOverlayCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& item : m_overlays) {
            count += item.second->GetOverlayCount();
        }
        return count;
    }

    void SetInputStamp(const RenderInputStamp stamp) const
    {
        for (const auto& item : m_featureViews) {
            item.second->SetInputStamp(stamp);
        }
    }

    void ClearFeaturePort(const std::string& viewId)
    {
        m_featureViews.erase(viewId);
    }

private:
    std::vector<HostFeatureView> m_views;
    std::unordered_map<std::string, std::shared_ptr<OverlayStub>> m_overlays;
    std::unordered_map<std::string, std::shared_ptr<FeatureViewStub>>
        m_featureViews;
};

class HostControlStub final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { return true; }

    bool SetActiveViews(
        const std::vector<std::string>& viewIds) override
    {
        if (m_failSetActiveViews) return false;
        m_activeViews = viewIds;
        ++m_setActiveCount;
        return true;
    }

    bool SetViewStatus(
        const std::vector<std::string>&,
        const std::string&) override
    {
        return true;
    }

    bool SendSceneDelta(FeatureSceneDelta delta) override
    {
        if (m_isSceneRejected || delta.requestId == 0
            || delta.viewIds.empty()) return false;
        m_sceneDeltas.push_back(std::move(delta));
        return true;
    }

    void SetSceneRejected(const bool isRejected) noexcept
    {
        m_isSceneRejected = isRejected;
    }

    std::size_t GetSceneDeltaCount() const noexcept
    {
        return m_sceneDeltas.size();
    }

    bool SendOwnerComplete(
        std::function<void()> complete) override
    {
        if (!complete) return false;
        ++m_ownerCompleteCount;
        if (m_deferOwnerCompletes) {
            m_ownerCompletes.push_back(std::move(complete));
        }
        else {
            complete();
        }
        return true;
    }

    void SetFailActiveViews(const bool fail) noexcept
    {
        m_failSetActiveViews = fail;
    }

    const std::vector<std::string>& GetActiveViews() const noexcept
    {
        return m_activeViews;
    }

    int GetOwnerCompleteCount() const noexcept
    {
        return m_ownerCompleteCount;
    }

    void SetDeferOwnerCompletes(const bool defer) noexcept
    {
        m_deferOwnerCompletes = defer;
    }

    std::size_t GetPendingOwnerCompleteCount() const noexcept
    {
        return m_ownerCompletes.size();
    }

    void SendOwnerCompletions()
    {
        auto completes = std::move(m_ownerCompletes);
        m_ownerCompletes.clear();
        for (auto& complete : completes) complete();
    }

private:
    std::vector<std::string> m_activeViews;
    std::vector<FeatureSceneDelta> m_sceneDeltas;
    int m_setActiveCount = 0;
    int m_ownerCompleteCount = 0;
    bool m_failSetActiveViews = false;
    bool m_isSceneRejected = false;
    bool m_deferOwnerCompletes = false;
    std::vector<std::function<void()>> m_ownerCompletes;
};

vtkSmartPointer<vtkImageData> BuildImage(const int side)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(side, side, side);
    image->SetSpacing(0.5, 0.75, 1.25);
    image->SetOrigin(1.0, 2.0, 3.0);
    image->AllocateScalars(VTK_FLOAT, 1);

    const std::size_t voxelCount = static_cast<std::size_t>(side)
        * static_cast<std::size_t>(side)
        * static_cast<std::size_t>(side);
    auto* values = static_cast<float*>(image->GetScalarPointer());
    std::fill(values, values + voxelCount, 0.0F);
    for (int z = 1; z <= 2; ++z) {
        for (int y = 1; y <= 2; ++y) {
            for (int x = 1; x <= 2; ++x) {
                const auto first = static_cast<std::size_t>(
                    x + side * (y + side * z));
                const int farX = side - 1 - x;
                const int farY = side - 1 - y;
                const int farZ = side - 1 - z;
                const auto second = static_cast<std::size_t>(
                    farX + side * (farY + side * farZ));
                values[first] = 1.0F;
                values[second] = 1.0F;
            }
        }
    }

    return image;
}

vtkSmartPointer<vtkImageData> BuildMask()
{
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->SetDimensions(8, 8, 8);
    mask->SetSpacing(0.5, 0.75, 1.25);
    mask->SetOrigin(1.0, 2.0, 3.0);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* values = static_cast<unsigned char*>(mask->GetScalarPointer());
    std::fill(values, values + 512, static_cast<unsigned char>(255));
    return mask;
}

vtkSmartPointer<vtkImageData> BuildCheckerImage()
{
    constexpr int side = 21;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(side, side, side);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* values = static_cast<float*>(image->GetScalarPointer());
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const auto index = static_cast<std::size_t>(
                    x + side * (y + side * z));
                values[index] = (x + y + z) % 2 == 0 ? 1.0F : 0.0F;
            }
        }
    }
    return image;
}

vtkSmartPointer<vtkImageData> BuildSoaImage()
{
    constexpr int side = 4;
    constexpr vtkIdType voxelCount = side * side * side;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(side, side, side);
    vtkNew<vtkSOADataArrayTemplate<float>> scalars;
    scalars->SetNumberOfComponents(1);
    scalars->SetNumberOfTuples(voxelCount);
    scalars->FillComponent(0, 1.0);
    image->GetPointData()->SetScalars(scalars);

    return image;
}

PartSegmentationConfig GetConfig(
    const std::size_t maxWorkingBytes = 512U * 1024U * 1024U)
{
    PartSegmentationConfig config;
    config.defaultStart.targetViews.viewRoles = {
        HostRenderViewRole::Primary3D,
        HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice
    };
    config.defaultStart.threshold = 0.5;
    config.defaultStart.minPartVoxels = 1;
    config.maxWorkingBytes = maxWorkingBytes;
    return config;
}

PartSegmentationRequest GetRequest(const PartSegmentationAction action)
{
    PartSegmentationRequest request;
    request.action = action;
    return request;
}

bool SendTicks(
    PartSegmentationHostFeature& feature,
    const std::function<bool()>& isComplete,
    const std::function<void()>& onTick = nullptr)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!isComplete() && std::chrono::steady_clock::now() < deadline) {
        if (!feature.OnHostTick()) return false;
        if (onTick) onTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return isComplete();
}

struct TestHost final {
    explicit TestHost(
        const int side = 8,
        const std::size_t maxWorkingBytes = 512U * 1024U * 1024U)
        : views(std::make_shared<ViewDirectoryStub>())
        , data(std::make_shared<TestDataPort>())
        , host(std::make_shared<HostControlStub>())
        , feature(std::make_shared<PartSegmentationHostFeature>(
            GetConfig(maxWorkingBytes)))
    {
        (void)data->SetPrimaryImage(BuildImage(side));
        const auto snapshot = data->GetPrimaryImage();
        if (snapshot && snapshot->data) views->SetInputStamp({ snapshot->data->self });
    }

    bool Attach()
    {
        const auto source = data->GetPrimaryImage();
        if (source && source->data) views->SetInputStamp({ source->data->self });
        HostFeatureContext context;
        context.views = views;
        context.data = data;
        context.host = host;
        return feature->AttachHost(context);
    }

    std::shared_ptr<ViewDirectoryStub> views;
    std::shared_ptr<TestDataPort> data;
    std::shared_ptr<HostControlStub> host;
    std::shared_ptr<PartSegmentationHostFeature> feature;
};

bool GetSurfaceRetentionValid()
{
    const auto config = GetConfig();
    TestDataPort probeData;
    const auto source = probeData.SetPrimaryImage(BuildImage(8));
    PartSegmentationService probe;
    const auto getComplete = [&probe]() {
        std::optional<PartLabelCandidate> result;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
        do {
            result = probe.GetComplete();
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!result && std::chrono::steady_clock::now() < deadline);
        return result;
    };
    if (probe.Start(source, config.defaultStart, config.maxWorkingBytes, 1)
        != PartAdmissionStatus::Accepted) return false;
    const auto first = getComplete();
    if (!first || first->status != PartResultStatus::Succeeded
        || !first->catalog || first->surfaceBytes == 0) return false;
    // 测出只保留历史标签/目录时足够的预算，再让真实 Feature 保留旧表面重算。
    if (probe.Start(source, config.defaultStart, config.maxWorkingBytes, 2,
            { first->labels, first->catalog },
            first->catalog->resultRevision, first->catalog->catalogRevision)
        != PartAdmissionStatus::Accepted) return false;
    const auto baseline = getComplete();
    if (!baseline || baseline->status != PartResultStatus::Succeeded
        || baseline->requiredBytes < first->requiredBytes) return false;

    // 实际 Host 还必须为图冻结与 VTK 视图预留空间；先取得首次完整发布的预算。
    TestHost publicationProbe;
    if (!publicationProbe.Attach()) return false;
    std::optional<PartSegmentationResult> publication;
    if (publicationProbe.feature->SendRequest(GetRequest(PartSegmentationAction::Start),
            [&publication](PartSegmentationResult value) { publication = std::move(value); })
                .status != PartAdmissionStatus::Accepted
        || !SendTicks(*publicationProbe.feature, [&] { return publication.has_value(); })
        || publication->status != PartResultStatus::Succeeded) return false;
    const std::string budgetKey = "graphPublicationBytes=";
    const auto budgetOffset = publication->message.find(budgetKey);
    if (budgetOffset == std::string::npos || !publicationProbe.feature->DetachHost()) return false;
    const auto publicationBudget = static_cast<std::size_t>(std::stoull(
        publication->message.substr(budgetOffset + budgetKey.size())));
    TestHost test(8, std::max(baseline->requiredBytes, publicationBudget));
    if (!test.Attach()) return false;
    auto result = std::make_shared<std::optional<PartSegmentationResult>>();
    const auto sendStart = [&]() {
        result->reset();
        return test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [result](PartSegmentationResult value) { *result = std::move(value); });
    };
    if (sendStart().status != PartAdmissionStatus::Accepted
        || !SendTicks(*test.feature, [&] { return result->has_value(); })
        || (*result)->status != PartResultStatus::Succeeded) return false;
    const auto active = test.feature->GetPartSetSnapshot();
    const auto overlayCount = test.views->GetOverlayCount();
    const auto graphBefore = test.data->GetDataGraph();
    const bool isAccepted = sendStart().status == PartAdmissionStatus::Accepted;
    const bool didComplete = SendTicks(
        *test.feature, [&] { return result->has_value(); });
    const bool isRetained = isAccepted && didComplete && active
        && (*result)->status == PartResultStatus::Failed
        && (*result)->failureReason == PartFailureReason::BudgetExceeded
        && test.feature->GetPartSetSnapshot() == active
        && test.feature->GetState().status == PartSegmentationStatus::Succeeded
        && test.data->GetDataGraph().commitId == graphBefore.commitId
        && overlayCount == 4 && test.views->GetOverlayCount() == overlayCount;
    return test.feature->DetachHost() && isRetained;
}

} // namespace

int GetPartLifecycleFailCount()
{
    int failureCount = 0;
    failureCount += GetCaseResult(
        GetSurfaceRetentionValid(),
        "Retained surface consumes recompute budget while preserving the active Part result")
        ? 0 : 1;

    {
        TestHost test;
        const auto ownerThread = std::this_thread::get_id();
        const bool isAttached = test.Attach();
        const auto primaryBefore = test.data->GetPrimaryImage();
        std::optional<PartSegmentationResult> startResult;
        int startCount = 0;
        std::thread::id callbackThread;
        const auto start = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                ++startCount;
                callbackThread = std::this_thread::get_id();
                startResult = std::move(result);
            });
        int duplicateCount = 0;
        const auto duplicate = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult) { ++duplicateCount; });
        std::vector<double> progressValues;
        const bool didComplete = SendTicks(
            *test.feature,
            [&] { return startResult.has_value(); },
            [&] {
                progressValues.push_back(test.feature->GetState().progress);
            });
        const auto state = test.feature->GetState();
        const auto firstSnapshot = test.feature->GetPartSetSnapshot();
        const void* const firstLabel =
            test.views->GetOverlay("part-top")->GetLabelPointer();

        failureCount += GetCaseResult(
            isAttached
                && start.status == PartAdmissionStatus::Accepted
                && start.requestId != 0
                && duplicate.status == PartAdmissionStatus::Busy
                && duplicate.requestId == 0
                && duplicateCount == 0,
            "Start admission is bounded and rejects a duplicate") ? 0 : 1;
        failureCount += GetCaseResult(
            didComplete && startCount == 1 && startResult
                && startResult->requestId == start.requestId
                && startResult->status == PartResultStatus::Succeeded
                && startResult->partCount == 2
                && startResult->message.find("peakWorkingBytes=")
                    != std::string::npos
                && startResult->message.find("surfaceBytes=")
                    != std::string::npos
                && callbackThread == ownerThread,
            "Accepted Start completes exactly once on the owner thread") ? 0 : 1;
        failureCount += GetCaseResult(
            state.status == PartSegmentationStatus::Succeeded
                && state.progress == 1.0
                && !progressValues.empty()
                && std::is_sorted(
                    progressValues.begin(), progressValues.end())
                && progressValues.back() == 1.0
                && state.partCount == 2
                && state.resultRevision == 1
                && state.catalogRevision == 1
                && firstSnapshot
                && firstSnapshot->partSetId == state.partSetId
                && firstSnapshot->resultRevision == 1
                && firstSnapshot->catalogRevision == 1
                && firstSnapshot->parts.size() == 2
                && firstSnapshot->partSetId != PartSetId{}
                && firstSnapshot->parts[0].binding.object.objectId
                    != PartObjectId{}
                && GetDataRevisionRefValid(state.labelMap)
                && GetDataRevisionRefValid(state.partTable)
                && GetDataRevisionRefValid(state.resultSet)
                && firstLabel != nullptr
                && test.views->GetOverlayCount() == 4
                && test.host->GetActiveViews().size() == 4,
            "Successful commit publishes one overlay per supported View") ? 0 : 1;
        failureCount += GetCaseResult(
            primaryBefore && test.data->GetPrimaryImage()
                && primaryBefore->data->self
                    == test.data->GetPrimaryImage()->data->self
                && primaryBefore->binding->revision
                    == test.data->GetPrimaryImage()->binding->revision,
            "PartSegmentation leaves the primary Binding unchanged") ? 0 : 1;

        const auto firstResultSet = state.resultSet;
        test.views->GetOverlay("part-top")->SetFailNextAttach();
        std::optional<PartSegmentationResult> replaceResult;
        const auto replace = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                replaceResult = std::move(result);
            });
        const bool didReplace = SendTicks(
            *test.feature, [&] { return replaceResult.has_value(); });
        const auto restored = test.feature->GetState();
        const auto restoredSnapshot = test.feature->GetPartSetSnapshot();
        const void* const restoredLabel =
            test.views->GetOverlay("part-top")->GetLabelPointer();
        failureCount += GetCaseResult(
            replace.status == PartAdmissionStatus::Accepted
                && didReplace && replaceResult
                && replaceResult->status
                    == PartResultStatus::SucceededWithDisplayFailure
                && replaceResult->failureReason
                    == PartFailureReason::DisplayFailed
                && restored.status == PartSegmentationStatus::Succeeded
                && restored.resultRevision == 2
                && restored.catalogRevision == 2
                && restoredSnapshot && restoredSnapshot != firstSnapshot
                && restoredSnapshot->partSetId == firstSnapshot->partSetId
                && restored.resultSet != firstResultSet
                && GetDataRevisionRefValid(restored.labelMap)
                && restoredLabel == nullptr
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Display failure preserves the newly committed data") ? 0 : 1;

        auto hide = GetRequest(PartSegmentationAction::SetVisibility);
        hide.isVisible = false;
        int hideCount = 0;
        PartSegmentationResult hideResult;
        const auto hideAdmission = test.feature->SendRequest(
            std::move(hide),
            [&](PartSegmentationResult result) {
                ++hideCount;
                hideResult = std::move(result);
            });
        failureCount += GetCaseResult(
            hideAdmission.status == PartAdmissionStatus::Accepted
                && hideCount == 1
                && hideResult.status == PartResultStatus::Succeeded
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty()
                && !test.feature->GetState().isOverlayVisible,
            "Visibility off removes all Part overlays") ? 0 : 1;

        auto show = GetRequest(PartSegmentationAction::SetVisibility);
        show.isVisible = true;
        int showCount = 0;
        const auto showAdmission = test.feature->SendRequest(
            std::move(show),
            [&](PartSegmentationResult result) {
                if (result.status == PartResultStatus::Succeeded) ++showCount;
            });
        failureCount += GetCaseResult(
            showAdmission.status == PartAdmissionStatus::Accepted
                && showCount == 1
                && test.views->GetOverlayCount() == 4
                && test.host->GetActiveViews().size() == 4
                && test.feature->GetState().isOverlayVisible
                && test.views->GetOverlay("part-top")->GetLabelPointer()
                    != nullptr,
            "Visibility on rebinds the committed label image") ? 0 : 1;

        std::optional<PartSegmentationResult> nextResult;
        const auto next = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                nextResult = std::move(result);
            });
        const bool didCommitNext = SendTicks(
            *test.feature, [&] { return nextResult.has_value(); });
        const void* const nextLabel =
            test.views->GetOverlay("part-top")->GetLabelPointer();
        const auto nextSnapshot = test.feature->GetPartSetSnapshot();
        failureCount += GetCaseResult(
            next.status == PartAdmissionStatus::Accepted
                && didCommitNext && nextResult
                && nextResult->status == PartResultStatus::Succeeded
                && nextResult->resultRevision == 3
                && nextSnapshot
                && nextSnapshot != firstSnapshot
                && nextSnapshot->partSetId == firstSnapshot->partSetId
                && nextSnapshot->catalogRevision == 3
                && nextSnapshot->parts[0].binding.object.objectId
                    == firstSnapshot->parts[0].binding.object.objectId
                && nextSnapshot->parts[1].binding.object.objectId
                    == firstSnapshot->parts[1].binding.object.objectId
                && nextResult->resultSet != restored.resultSet
                && nextLabel != nullptr
                && GetDataRevisionRefValid(nextResult->partTable),
            "Successful replacement atomically swaps the label generation")
            ? 0 : 1;

        int clearCount = 0;
        const auto clear = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Clear),
            [&](PartSegmentationResult result) {
                if (result.status == PartResultStatus::Succeeded) ++clearCount;
            });
        const auto cleared = test.feature->GetState();
        failureCount += GetCaseResult(
            clear.status == PartAdmissionStatus::Accepted
                && clearCount == 1
                && cleared.status == PartSegmentationStatus::Idle
                && cleared.partCount == 0
                && !test.feature->GetPartSetSnapshot()
                && !GetDataRevisionRefValid(cleared.resultSet)
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Clear retires the catalog and display") ? 0 : 1;
        std::optional<PartSegmentationResult> restartedResult;
        const auto restarted = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                restartedResult = std::move(result);
            });
        const bool didRestart = SendTicks(
            *test.feature, [&] { return restartedResult.has_value(); });
        const auto restartedSnapshot = test.feature->GetPartSetSnapshot();
        failureCount += GetCaseResult(
            restarted.status == PartAdmissionStatus::Accepted
                && didRestart && restartedResult
                && restartedResult->status == PartResultStatus::Succeeded
                && restartedSnapshot
                && restartedSnapshot->partSetId != firstSnapshot->partSetId
                && restartedSnapshot->resultRevision == 1
                && restartedSnapshot->catalogRevision == 1,
            "Clear makes the next result a new PartSet generation") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Clean lifecycle detaches successfully") ? 0 : 1;
    }

    {
        TestHost test(8, 1);
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->failureReason == PartFailureReason::BudgetExceeded
                && result->message.find("requiredBytes=")
                    != std::string::npos
                && result->message.find("maxWorkingBytes=1")
                    != std::string::npos
                && test.feature->GetState().progress < 1.0,
            "Budget failure reports exact required and configured bytes")
            ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Budget failure remains detachable") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> firstResult;
        const auto first = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                firstResult = std::move(result);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return firstResult.has_value(); });
        const auto firstSnapshot = test.feature->GetPartSetSnapshot();
        PartStatePatch namePatch;
        namePatch.name = "retained name";
        namePatch.isReviewed = true;
        const auto named = firstSnapshot
            ? test.feature->SetPartState(
                firstSnapshot->parts[0].binding,
                namePatch,
                firstSnapshot->catalogRevision)
            : PartMutationResult{};
        const auto namedSnapshot = test.feature->GetPartSetSnapshot();
        const auto namedAgain = namedSnapshot
            ? test.feature->SetPartState(
                namedSnapshot->parts[0].binding,
                namePatch,
                namedSnapshot->catalogRevision)
            : PartMutationResult{};
        failureCount += GetCaseResult(
            isAttached
                && first.status == PartAdmissionStatus::Accepted
                && didComplete && firstResult && firstSnapshot
                && named.status == PartMutationStatus::Succeeded
                && named.catalogRevision == 2
                && namedSnapshot
                && namedSnapshot != firstSnapshot
                && namedSnapshot->resultRevision == 1
                && namedSnapshot->parts[0].userState.name == "retained name"
                && namedSnapshot->parts[0].userState.isReviewed
                && namedAgain.status == PartMutationStatus::Succeeded
                && namedAgain.catalogRevision == 2,
            "Part state mutation is immutable, versioned, and idempotent")
            ? 0 : 1;

        PartStatePatch selectPatch;
        selectPatch.isSelected = true;
        const auto previousDeltaCount = test.host->GetSceneDeltaCount();
        test.host->SetSceneRejected(true);
        const auto rejectedSelection = test.feature->SetPartState(
            namedSnapshot->parts[0].binding,
            selectPatch,
            namedSnapshot->catalogRevision);
        test.host->SetSceneRejected(false);
        failureCount += GetCaseResult(
            rejectedSelection.status == PartMutationStatus::DisplayFailed
                && rejectedSelection.catalogRevision
                    == namedSnapshot->catalogRevision
                && test.feature->GetPartSetSnapshot() == namedSnapshot
                && test.host->GetSceneDeltaCount() == previousDeltaCount,
            "Rejected frame intent preserves the Part catalog") ? 0 : 1;
        const auto selectedFirst = test.feature->SetPartState(
            namedSnapshot->parts[0].binding,
            selectPatch,
            namedSnapshot->catalogRevision);
        const auto selectedSnapshot = test.feature->GetPartSetSnapshot();
        const auto selectedSecond = test.feature->SetPartState(
            selectedSnapshot->parts[1].binding,
            selectPatch,
            selectedSnapshot->catalogRevision);
        const auto finalSelection = test.feature->GetPartSetSnapshot();
        failureCount += GetCaseResult(
            selectedFirst.status == PartMutationStatus::Succeeded
                && selectedSecond.status == PartMutationStatus::Succeeded
                && test.host->GetSceneDeltaCount() == previousDeltaCount + 2
                && finalSelection
                && !finalSelection->parts[0].presentation.isSelected
                && finalSelection->parts[1].presentation.isSelected
                && test.feature->GetState().resultRevision == 1,
            "Part selection remains singular without changing result revision")
            ? 0 : 1;

        PartMutationResult wrongThread;
        std::thread mutationCaller([&] {
            PartStatePatch patch;
            patch.isVisible = false;
            wrongThread = test.feature->SetPartState(
                finalSelection->parts[0].binding,
                patch,
                finalSelection->catalogRevision);
        });
        mutationCaller.join();

        std::optional<PartSegmentationResult> replacementResult;
        const auto replacement = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                replacementResult = std::move(result);
            });
        PartStatePatch busyPatch;
        busyPatch.isVisible = false;
        const auto busyMutation = test.feature->SetPartState(
            finalSelection->parts[0].binding,
            busyPatch,
            finalSelection->catalogRevision);
        const bool didReplace = SendTicks(
            *test.feature, [&] { return replacementResult.has_value(); });
        const auto replacedSnapshot = test.feature->GetPartSetSnapshot();
        failureCount += GetCaseResult(
            wrongThread.status == PartMutationStatus::Unavailable
                && replacement.status == PartAdmissionStatus::Accepted
                && busyMutation.status == PartMutationStatus::Busy
                && didReplace && replacementResult && replacedSnapshot
                && replacedSnapshot->resultRevision == 2
                && replacedSnapshot->catalogRevision == 5
                && replacedSnapshot->parts[0].userState.name
                    == "retained name"
                && replacedSnapshot->parts[0].userState.isReviewed
                && replacedSnapshot->parts[1].presentation.isSelected,
            "Exact continuation retains state and rejects concurrent mutation")
            ? 0 : 1;

        test.data->SetPrimaryImage(BuildImage(8));
        const bool didStale = test.feature->OnHostTick();
        const auto staleSnapshot = test.feature->GetPartSetSnapshot();
        const auto staleMutation = staleSnapshot
            ? test.feature->SetPartState(
                staleSnapshot->parts[0].binding,
                busyPatch,
                staleSnapshot->catalogRevision)
            : PartMutationResult{};
        failureCount += GetCaseResult(
            didStale && staleSnapshot && staleSnapshot->isStale
                && staleMutation.status
                    == PartMutationStatus::StaleReference
                && test.feature->DetachHost(),
            "Stale snapshot remains readable but rejects mutation") ? 0 : 1;
    }

    {
        TestHost test;
        test.data->SetPrimaryImage(BuildSoaImage());
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Succeeded
                && result->failureReason == PartFailureReason::None
                && GetDataRevisionRefValid(result->resultSet)
                && test.views->GetOverlayCount() == 4,
            "DataGraph bridge normalizes non-standard scalar layout")
            ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Normalized scalar layout remains detachable") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        test.views->GetOverlay("part-top")->SetFailNextAttach();
        std::optional<PartSegmentationResult> failedResult;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                failedResult = std::move(result);
            });
        const bool didFail = SendTicks(
            *test.feature, [&] { return failedResult.has_value(); });
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didFail && failedResult
                && failedResult->status
                    == PartResultStatus::SucceededWithDisplayFailure
                && failedResult->failureReason
                    == PartFailureReason::DisplayFailed
                && GetDataRevisionRefValid(failedResult->resultSet)
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Partial overlay failure does not roll back committed data") ? 0 : 1;

        std::optional<PartSegmentationResult> retryResult;
        const auto retry = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                retryResult = std::move(result);
            });
        const bool didRetry = SendTicks(
            *test.feature, [&] { return retryResult.has_value(); });
        failureCount += GetCaseResult(
            retry.status == PartAdmissionStatus::Accepted
                && didRetry && retryResult
                && retryResult->status == PartResultStatus::Succeeded
                && test.views->GetOverlayCount() == 4,
            "Display failure leaves the Feature retryable") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Retried lifecycle detaches successfully") ? 0 : 1;
    }

    {
        TestHost test;
        (void)test.data->SetPrimaryImage(BuildImage(8), BuildMask());
        const bool isAttached = test.Attach();
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto pendingClear = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Clear));
        const bool didComplete = SendTicks(
            *test.feature,
            [&] {
                return test.feature->GetState().status
                    != PartSegmentationStatus::Running;
            });
        const auto state = test.feature->GetState();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && pendingClear.status == PartAdmissionStatus::Busy
                && didComplete
                && state.status == PartSegmentationStatus::Succeeded
                && state.partCount == 2
                && GetDataRevisionRefValid(state.partTable)
                && test.views->GetOverlayCount() == 4,
            "Finite nonzero masks keep voxels without integer truncation") ? 0 : 1;
        const auto clear = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Clear));
        failureCount += GetCaseResult(
            clear.status == PartAdmissionStatus::Accepted
                && test.feature->GetState().status
                    == PartSegmentationStatus::Idle
                && test.feature->DetachHost(),
            "A null Start callback does not retain or wedge the request") ? 0 : 1;
    }

    {
        TestHost test;
        (void)test.data->SetPrimaryImage(BuildCheckerImage());
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        const auto state = test.feature->GetState();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Failed
                && result->failureReason == PartFailureReason::BudgetExceeded
                && state.status == PartSegmentationStatus::Failed
                && state.failureReason == PartFailureReason::BudgetExceeded
                && test.views->GetOverlayCount() == 0,
            "Part-count limit rejects an unbounded overlay candidate") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Overlay-limit failure remains detachable") ? 0 : 1;
    }

    {
        TestHost test(64);
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> changedResult;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                changedResult = std::move(result);
            });
        (void)test.data->SetPrimaryImage(BuildImage(8));
        const bool didComplete = SendTicks(
            *test.feature, [&] { return changedResult.has_value(); });
        const auto state = test.feature->GetState();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && changedResult
                && changedResult->status == PartResultStatus::Failed
                && changedResult->failureReason
                    == PartFailureReason::SourceChanged
                && state.status == PartSegmentationStatus::Stale
                && state.failureReason == PartFailureReason::SourceChanged
                && test.views->GetOverlayCount() == 0,
            "Source identity change rejects a late candidate") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Stale lifecycle remains read-only and detachable") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        (void)test.data->SetPrimaryImage(BuildImage(8));
        const bool staleTick = test.feature->OnHostTick();
        const auto staleSnapshot = test.feature->GetPartSetSnapshot();
        auto hide = GetRequest(PartSegmentationAction::SetVisibility);
        hide.isVisible = false;
        const auto hideAdmission = test.feature->SendRequest(
            std::move(hide));
        auto show = GetRequest(PartSegmentationAction::SetVisibility);
        show.isVisible = true;
        const auto showAdmission = test.feature->SendRequest(
            std::move(show));
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Succeeded
                && staleTick
                && test.feature->GetState().status
                    == PartSegmentationStatus::Stale
                && staleSnapshot
                && staleSnapshot->isStale
                && staleSnapshot->sourceRevision == result->sourceRevision
                && hideAdmission.status == PartAdmissionStatus::Accepted
                && showAdmission.status == PartAdmissionStatus::Accepted
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Stale labels cannot be made visible again") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Retired stale display detaches successfully") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        test.host->SetFailActiveViews(true);
        (void)test.data->SetPrimaryImage(BuildImage(8));
        const bool firstStaleTick = test.feature->OnHostTick();
        const auto pendingCleanup = test.feature->GetState();
        const bool retiredLocalDisplay =
            test.views->GetOverlayCount() == 0
            && test.host->GetActiveViews().size() == 4
            && test.views->GetOverlay("part-top")->GetLabelPointer()
                == nullptr;
        test.host->SetFailActiveViews(false);
        const bool retryStaleTick = test.feature->OnHostTick();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Succeeded
                && firstStaleTick
                && pendingCleanup.status == PartSegmentationStatus::Stale
                && pendingCleanup.progress == 0.0
                && retiredLocalDisplay
                && retryStaleTick
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Stale cleanup retires local overlays and retries Host metadata")
            ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Retried stale cleanup remains detachable") ? 0 : 1;
    }

    {
        TestHost test(8);
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> startResult;
        int startCount = 0;
        const auto start = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                ++startCount;
                startResult = std::move(result);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int stopCount = 0;
        const auto stop = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Stop),
            [&](PartSegmentationResult result) {
                if (result.status == PartResultStatus::Succeeded) ++stopCount;
            });
        const bool didCancel = SendTicks(
            *test.feature, [&] { return startResult.has_value(); });
        failureCount += GetCaseResult(
            isAttached
                && start.status == PartAdmissionStatus::Accepted
                && stop.status == PartAdmissionStatus::Accepted
                && stopCount == 1
                && didCancel && startCount == 1 && startResult
                && startResult->status == PartResultStatus::Cancelled
                && startResult->failureReason == PartFailureReason::Cancelled
                && test.feature->GetState().status
                    == PartSegmentationStatus::Cancelled
                && test.feature->GetState().progress < 1.0,
            "Stop cancels an accepted but uncommitted candidate") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Cancelled lifecycle detaches successfully") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        test.host->SetDeferOwnerCompletes(true);
        std::optional<PartSegmentationResult> result;
        int callbackCount = 0;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                ++callbackCount;
                result = std::move(value);
            });
        const bool didQueue = SendTicks(
            *test.feature,
            [&] {
                return test.host->GetPendingOwnerCompleteCount() == 1;
            });
        test.views->ClearFeaturePort("part-top");
        test.host->SendOwnerCompletions();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didQueue && callbackCount == 1 && result
                && result->status == PartResultStatus::Failed
                && result->failureReason
                    == PartFailureReason::DisplayFailed,
            "Rendered completion rejects a detached target View") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "View-stale completion remains detachable") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        PartSegmentationAdmission otherThread;
        std::thread caller([&] {
            otherThread = test.feature->SendRequest(
                GetRequest(PartSegmentationAction::Clear));
        });
        caller.join();
        failureCount += GetCaseResult(
            isAttached
                && otherThread.status == PartAdmissionStatus::Unavailable,
            "Requests from a non-owner thread are rejected") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Owner thread can still detach after a rejected request") ? 0 : 1;
    }

    {
        TestHost test;
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> result;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult value) {
                result = std::move(value);
            });
        const bool didComplete = SendTicks(
            *test.feature, [&] { return result.has_value(); });
        test.host->SetFailActiveViews(true);
        const bool firstDetach = test.feature->DetachHost();
        const auto stateAfterFailure = test.feature->GetState();
        const bool clearedLocally =
            test.views->GetOverlayCount() == 0
            && test.host->GetActiveViews().size() == 4
            && stateAfterFailure.status == PartSegmentationStatus::Idle
            && !GetDataRevisionRefValid(stateAfterFailure.resultSet);
        test.host->SetFailActiveViews(false);
        const bool retryDetach = test.feature->DetachHost();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Succeeded
                && !firstDetach && clearedLocally
                && retryDetach
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Detach failure retires local data and retries Host metadata") ? 0 : 1;
    }

    return failureCount;
}
