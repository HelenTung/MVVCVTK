#include "PartSegmentationTestCases.h"

#include "App/Services/FeatureViewService.h"
#include "Host/PartSegmentationHostFeature.h"
#include "Render/Contracts/OverlayService.h"

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

class DataPortStub final : public TrustedFeatureDataPort {
public:
    explicit DataPortStub(TrustedImageSnapshot snapshot)
        : m_snapshot(std::move(snapshot))
    {
    }

    TrustedImageSnapshot GetImageSnapshot() const override
    {
        return m_snapshot;
    }

    bool SetImageState(
        TrustedImageState,
        const TrustedImageSnapshot&,
        TrustedImageSnapshot&) override
    {
        ++m_setCount;
        return false;
    }

    void SetSnapshot(TrustedImageSnapshot snapshot)
    {
        m_snapshot = std::move(snapshot);
    }

    int GetSetCount() const noexcept { return m_setCount; }

private:
    TrustedImageSnapshot m_snapshot;
    int m_setCount = 0;
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
        if (delta.requestId == 0 || delta.viewIds.empty()) return false;
        m_sceneDeltas.push_back(std::move(delta));
        return true;
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
    bool m_deferOwnerCompletes = false;
    std::vector<std::function<void()>> m_ownerCompletes;
};

TrustedImageSnapshot BuildSnapshot(
    const int side,
    const DataVersion version)
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

    auto state = std::make_shared<TrustedImageState>();
    state->image = std::move(image);
    state->dims = { side, side, side };
    state->spacing = { 0.5, 0.75, 1.25 };
    state->origin = { 1.0, 2.0, 3.0 };
    state->scalarRange = { 0.0, 1.0 };
    state->version = version;
    return state;
}

TrustedImageSnapshot BuildMaskSnapshot()
{
    auto state = std::make_shared<TrustedImageState>(
        *BuildSnapshot(8, 3));
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->SetDimensions(8, 8, 8);
    mask->SetSpacing(0.5, 0.75, 1.25);
    mask->SetOrigin(1.0, 2.0, 3.0);
    mask->AllocateScalars(VTK_UNSIGNED_SHORT, 1);
    auto* values = static_cast<unsigned short*>(mask->GetScalarPointer());
    std::fill(values, values + 512, static_cast<unsigned short>(256));
    state->validityMask = std::move(mask);
    return state;
}

TrustedImageSnapshot BuildCheckerSnapshot()
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
    auto state = std::make_shared<TrustedImageState>();
    state->image = std::move(image);
    state->dims = { side, side, side };
    state->scalarRange = { 0.0, 1.0 };
    state->version = 4;
    return state;
}

TrustedImageSnapshot BuildSoaSnapshot()
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

    auto state = std::make_shared<TrustedImageState>();
    state->image = std::move(image);
    state->dims = { side, side, side };
    state->scalarRange = { 1.0, 1.0 };
    state->version = 5;
    return state;
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
        const DataVersion version = 1,
        const std::size_t maxWorkingBytes = 512U * 1024U * 1024U)
        : views(std::make_shared<ViewDirectoryStub>())
        , data(std::make_shared<DataPortStub>(BuildSnapshot(side, version)))
        , host(std::make_shared<HostControlStub>())
        , feature(std::make_shared<PartSegmentationHostFeature>(
            GetConfig(maxWorkingBytes)))
    {
        const auto snapshot = data->GetImageSnapshot();
        if (snapshot) {
            views->SetInputStamp({
                snapshot->image.GetPointer(), snapshot->version });
        }
    }

    bool Attach()
    {
        HostFeatureContext context;
        context.views = views;
        context.data = data;
        context.host = host;
        return feature->AttachHost(context);
    }

    std::shared_ptr<ViewDirectoryStub> views;
    std::shared_ptr<DataPortStub> data;
    std::shared_ptr<HostControlStub> host;
    std::shared_ptr<PartSegmentationHostFeature> feature;
};

} // namespace

int GetPartLifecycleFailCount()
{
    int failureCount = 0;

    {
        TestHost test;
        const auto ownerThread = std::this_thread::get_id();
        const bool isAttached = test.Attach();
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
                && callbackThread == ownerThread,
            "Accepted Start completes exactly once on the owner thread") ? 0 : 1;
        failureCount += GetCaseResult(
            state.status == PartSegmentationStatus::Succeeded
                && state.progress == 1.0
                && !progressValues.empty()
                && std::is_sorted(
                    progressValues.begin(), progressValues.end())
                && progressValues.back() == 1.0
                && state.parts.size() == 2
                && state.resultRevision == 1
                && firstLabel != nullptr
                && test.views->GetOverlayCount() == 4
                && test.host->GetActiveViews().size() == 4,
            "Successful commit publishes one overlay per supported View") ? 0 : 1;
        failureCount += GetCaseResult(
            test.data->GetSetCount() == 0,
            "PartSegmentation never replaces Host image state") ? 0 : 1;

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
        const void* const restoredLabel =
            test.views->GetOverlay("part-top")->GetLabelPointer();
        failureCount += GetCaseResult(
            replace.status == PartAdmissionStatus::Accepted
                && didReplace && replaceResult
                && replaceResult->status == PartResultStatus::Failed
                && replaceResult->failureReason
                    == PartFailureReason::DisplayFailed
                && restored.status == PartSegmentationStatus::Succeeded
                && restored.resultRevision == 1
                && restored.parts.size() == 2
                && restoredLabel == firstLabel
                && test.views->GetOverlayCount() == 4
                && test.host->GetActiveViews().size() == 4,
            "Failed replacement preserves the committed result") ? 0 : 1;

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
                    == firstLabel,
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
        failureCount += GetCaseResult(
            next.status == PartAdmissionStatus::Accepted
                && didCommitNext && nextResult
                && nextResult->status == PartResultStatus::Succeeded
                && nextResult->resultRevision == 2
                && nextLabel != nullptr
                && nextLabel != firstLabel,
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
                && cleared.parts.empty()
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Clear retires the catalog and display") ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Clean lifecycle detaches successfully") ? 0 : 1;
    }

    {
        TestHost test(8, 6, 1);
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
        test.data->SetSnapshot(BuildSoaSnapshot());
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
                && result->status == PartResultStatus::Failed
                && result->failureReason
                    == PartFailureReason::UnsupportedScalar
                && test.views->GetOverlayCount() == 0,
            "Non-standard scalar layout is rejected without staging")
            ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Unsupported scalar failure remains detachable") ? 0 : 1;
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
                && failedResult->status == PartResultStatus::Failed
                && failedResult->failureReason
                    == PartFailureReason::DisplayFailed
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Partial overlay attach rolls back the candidate") ? 0 : 1;

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
        test.data->SetSnapshot(BuildMaskSnapshot());
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
                && state.parts.size() == 2
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
        test.data->SetSnapshot(BuildCheckerSnapshot());
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
        TestHost test(64, 7);
        const bool isAttached = test.Attach();
        std::optional<PartSegmentationResult> changedResult;
        const auto admission = test.feature->SendRequest(
            GetRequest(PartSegmentationAction::Start),
            [&](PartSegmentationResult result) {
                changedResult = std::move(result);
            });
        test.data->SetSnapshot(BuildSnapshot(8, 8));
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
            test.data->GetSetCount() == 0 && test.feature->DetachHost(),
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
        test.data->SetSnapshot(BuildSnapshot(8, 2));
        const bool staleTick = test.feature->OnHostTick();
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
        const void* const committedLabel =
            test.views->GetOverlay("part-top")->GetLabelPointer();
        test.host->SetFailActiveViews(true);
        test.data->SetSnapshot(BuildSnapshot(8, 9));
        const bool firstStaleTick = test.feature->OnHostTick();
        const auto pendingCleanup = test.feature->GetState();
        const bool keptGeneration =
            test.views->GetOverlayCount() == 4
            && test.host->GetActiveViews().size() == 4
            && test.views->GetOverlay("part-top")->GetLabelPointer()
                == committedLabel;
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
                && keptGeneration
                && retryStaleTick
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Stale cleanup failure keeps a complete generation and retries")
            ? 0 : 1;
        failureCount += GetCaseResult(
            test.feature->DetachHost(),
            "Retried stale cleanup remains detachable") ? 0 : 1;
    }

    {
        TestHost test(8, 11);
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
        const bool keptBindings = test.views->GetOverlayCount() == 4;
        test.host->SetFailActiveViews(false);
        const bool retryDetach = test.feature->DetachHost();
        failureCount += GetCaseResult(
            isAttached
                && admission.status == PartAdmissionStatus::Accepted
                && didComplete && result
                && result->status == PartResultStatus::Succeeded
                && !firstDetach && keptBindings
                && retryDetach
                && test.views->GetOverlayCount() == 0
                && test.host->GetActiveViews().empty(),
            "Detach failure preserves bindings and succeeds on retry") ? 0 : 1;
    }

    return failureCount;
}
