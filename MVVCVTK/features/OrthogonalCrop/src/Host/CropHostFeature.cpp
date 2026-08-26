#include "Host/CropHostFeature.h"

#include "Interaction/CropBridge.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kFeatureId = "OrthogonalCrop";
constexpr std::size_t kCommandKeyCount = 10;
constexpr std::size_t kNodeKeyCount =
    std::tuple_size_v<decltype(CropHostKeys::nodes)>;

bool GetImageReady(vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = { 0, 0, 0 };
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

bool GetCharMatched(
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

bool GetChordMatched(
    const InteractionEvent& event,
    const HostKeyChord& chord)
{
    const bool hasKey = GetCharMatched(event, chord.keyCode)
        || (!chord.keySym.empty()
            && event.keySym == chord.keySym);
    return hasKey
        && event.isCtrlDown == chord.isCtrlDown
        && event.isAltDown == chord.isAltDown
        && event.isShiftDown == chord.isShiftDown;
}
}

class CropHostFeature::Impl final {
public:
    struct CompleteItem final {
        CropBuildCallback onComplete;
        std::optional<CropBuildResult> result;
        std::optional<RenderInputStamp> waitInput;
        bool isQueued = false;
    };

    struct CompleteState final {
        std::mutex mutex;
        std::vector<std::shared_ptr<CompleteItem>> items;
        bool isActive = true;
    };

    struct PublishGuard final {
        explicit PublishGuard(bool& isPublishing)
            : m_isPublishing(isPublishing)
        {
            m_isPublishing = true;
        }

        ~PublishGuard()
        {
            m_isPublishing = false;
        }

    private:
        bool& m_isPublishing;
    };

    explicit Impl(CropHostConfig config)
        : m_config(std::move(config))
        , m_bridge(std::make_unique<CropBridge>())
    {
    }

    bool AttachHost(
        CropHostFeature& owner,
        const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    bool SendRequest(
        CropHostRequest request,
        CropBuildCallback onComplete);
    CropHostState GetState() const;

private:
    bool StartCrop(const CropHostTarget& target);
    std::optional<CropInputSnapshot> GetCropInput(
        const CropHostTarget& target) const;
    bool GetTargetsValid(
        const HostViewTargets& targets) const;
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool SetCropInput(const CropHostTarget& target);
    bool SetPolyData(
        vtkSmartPointer<vtkPolyData> polyData,
        std::uint64_t sourceVersion);
    bool ClearPolyData();
    bool ResetOriginal();
    bool SetImageResult(
        const TrustedImageSnapshot& sourceSnapshot,
        const TrustedImageSnapshot& expectedSnapshot,
        CropBuildResult& result);
    bool BuildCropResult(
        const CropHostTarget& target,
        CropBuildCallback onComplete);
    static bool RemoveComplete(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item);
    static bool SetCompleteResult(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item,
        CropBuildResult result,
        std::optional<RenderInputStamp> waitInput);
    static bool SendComplete(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item);
    bool SendReadyCompletes();
    InteractionResult OnInput(const InteractionEvent& event);
    std::optional<std::size_t> GetKeyIndex(
        const InteractionEvent& event) const;
    CropHostRequest GetKeyRequest(
        std::size_t keyIndex) const;
    bool SendActionLog(
        const char* action,
        bool isAccepted) const;
    static const char* GetActionText(
        CropHostAction action);
    static const char* GetModeText(
        CropRemovalMode removalMode);
    static std::string GetHistoryText(
        const CropHistoryState& state);
    void SendStatus();
    void ClearBorrowed();

    CropHostConfig m_config;
    std::unique_ptr<CropBridge> m_bridge;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    vtkSmartPointer<vtkPolyData> m_polyData;
    std::uint64_t m_sourceVersion = 0;
    TrustedImageSnapshot m_rootImage;
    TrustedImageSnapshot m_lastImage;
    std::optional<CropHostTarget> m_activeTarget;
    std::vector<std::string> m_activeViewIds;
    std::optional<CropHistoryState> m_status;
    std::shared_ptr<CompleteState> m_completeState;
    std::thread::id m_ownerThread;
    std::array<bool,
        kCommandKeyCount + kNodeKeyCount> m_isDown{};
    bool m_isPublishing = false;
    bool m_isAttached = false;
};

bool CropHostFeature::Impl::AttachHost(
    CropHostFeature& owner,
    const HostFeatureContext& context)
{
    if (m_isAttached) {
        return m_views == context.views
            && m_data == context.data
            && m_host == context.host
            && m_ownerThread == std::this_thread::get_id();
    }
    if (owner.weak_from_this().expired()
        || !context.views
        || !context.data
        || !context.host) {
        return false;
    }

    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_completeState = std::make_shared<CompleteState>();
    m_ownerThread = std::this_thread::get_id();

    if (!m_config.inputViews.viewIds.empty()
        || !m_config.inputViews.viewRoles.empty()) {
        std::weak_ptr<CropHostFeature> weakOwner =
            owner.weak_from_this();
        HostInputBinding binding;
        binding.featureId = std::string(kFeatureId);
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
    }
    m_isAttached = true;
    return true;
}

bool CropHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) {
        return true;
    }
    if (m_ownerThread != std::this_thread::get_id()) {
        return false;
    }

    bool isInputDetached =
        m_config.inputViews.viewIds.empty()
        && m_config.inputViews.viewRoles.empty();
    if (!isInputDetached && m_host) {
        try {
            isInputDetached =
                m_host->DetachInput(kFeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
    }

    // 输入解绑失败时 Feature 仍留在 Session 注册表等待重试，但可见状态和质量来源必须先退休，
    // 避免宿主关闭流程因一个输入端口异常而长期保持 Crop/Quality。
    if (m_completeState) {
        const std::lock_guard<std::mutex> lock(
            m_completeState->mutex);
        m_completeState->isActive = false;
        m_completeState->items.clear();
    }
    if (m_bridge) {
        if (m_bridge->GetCropActive()) {
            (void)m_bridge->ExitCrop();
        }
        (void)m_bridge->ClearBindings();
    }
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_status.reset();
    m_isDown.fill(false);
    if (!isInputDetached) {
        return false;
    }
    m_isAttached = false;
    ClearBorrowed();
    return true;
}

void CropHostFeature::Impl::ClearBorrowed()
{
    if (m_host) {
        (void)m_host->SetActiveViews({});
    }
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    if (m_completeState) {
        const std::lock_guard<std::mutex> lock(
            m_completeState->mutex);
        m_completeState->isActive = false;
        m_completeState->items.clear();
    }
    m_completeState.reset();
    m_rootImage.reset();
    m_lastImage.reset();
}

bool CropHostFeature::Impl::SetCropInput(
    const CropHostTarget& target)
{
    auto input = GetCropInput(target);
    if (!m_bridge || !input) {
        return false;
    }

    const bool hasNewLineage =
        target.source == CropHostSource::CurrentImage
        && m_lastImage
        && (m_lastImage->version != input->inputVersion
            || m_lastImage->image.GetPointer()
                != input->imageData.GetPointer());
    if (!m_bridge->SetCropInput(std::move(*input))) {
        return false;
    }
    if (hasNewLineage) {
        // 外部 File/Reload 已提交新 current；只有 Bridge 接受新输入后才退休旧物化 lineage。
        m_rootImage.reset();
        m_lastImage.reset();
    }
    return true;
}

std::optional<CropInputSnapshot>
CropHostFeature::Impl::GetCropInput(
    const CropHostTarget& target) const
{
    CropInputSnapshot input;
    if (target.source == CropHostSource::CurrentImage) {
        const auto imageState = m_data
            ? m_data->GetImageSnapshot() : TrustedImageSnapshot{};
        if (!imageState || !GetImageReady(imageState->image)) {
            return std::nullopt;
        }
        input.dataSource =
            OrthogonalCropDataSource::ImageData;
        input.inputVersion = imageState->version;
        input.imageData = imageState->image;
        input.validityMask = imageState->validityMask;
        input.imageData->GetBounds(
            input.inputModelBounds.data());
    }
    else {
        if (!m_polyData || m_sourceVersion == 0) {
            return std::nullopt;
        }
        input.dataSource =
            OrthogonalCropDataSource::PolyData;
        input.inputVersion = m_sourceVersion;
        input.polyData = m_polyData;
        input.polyData->GetBounds(
            input.inputModelBounds.data());
    }
    return input;
}

bool CropHostFeature::Impl::GetTargetsValid(
    const HostViewTargets& targets) const
{
    if (!m_views
        || (targets.viewIds.empty()
            && targets.viewRoles.empty())) {
        return false;
    }
    const auto views = m_views->GetViews(targets);
    if (views.empty()) {
        return false;
    }
    for (const auto& viewId : targets.viewIds) {
        if (viewId.empty()
            || std::none_of(
                views.begin(), views.end(),
                [&viewId](const HostFeatureView& view) {
                    return view.id == viewId;
                })) {
            return false;
        }
    }
    for (const auto role : targets.viewRoles) {
        if (std::none_of(
                views.begin(), views.end(),
                [role](const HostFeatureView& view) {
                    return view.role == role;
                })) {
            return false;
        }
    }
    return true;
}

bool CropHostFeature::Impl::SetActiveViews(
    const std::vector<std::string>& viewIds) const
{
    return m_host
        && m_host->SetActiveViews(viewIds);
}

bool CropHostFeature::Impl::StartCrop(
    const CropHostTarget& target)
{
    if (!m_isAttached
        || !m_views
        || !m_bridge
        || (target.referenceView.viewId.empty()
            && !target.referenceView.isViewRoleUsed)) {
        return false;
    }
    const auto referenceView =
        m_views->GetInputView(target.referenceView);
    if (!referenceView) {
        return false;
    }
    const auto referencePort =
        m_views->GetFeaturePort(referenceView->view.id);
    if (!referencePort) return false;

    const auto& requestedTargets = target.isTargetViewsUsed
        ? target.targetViews
        : m_config.defaultTarget.targetViews;
    if (!GetTargetsValid(requestedTargets)) {
        return false;
    }
    const auto targetViews = m_views->GetViews(requestedTargets);
    auto input = GetCropInput(target);
    if (targetViews.empty() || !input) {
        return false;
    }

    CropViewRequest request;
    request.interactor = referenceView->interactor;
    request.renderer = referenceView->renderer;
    request.lease = referenceView->lease;
    request.referenceService = referencePort;
    std::vector<std::string> activeViewIds;
    activeViewIds.reserve(targetViews.size() + 1);
    for (const auto& view : targetViews) {
        const auto port = m_views->GetFeaturePort(view.id);
        if (port) {
            request.targetServices.push_back(port);
            if (!view.id.empty()
                && std::find(
                    activeViewIds.begin(),
                    activeViewIds.end(),
                    view.id) == activeViewIds.end()) {
                activeViewIds.push_back(view.id);
            }
        }
    }
    if (request.targetServices.empty()) {
        return false;
    }
    if (std::find(
            activeViewIds.begin(),
            activeViewIds.end(),
            referenceView->view.id)
            == activeViewIds.end()) {
        activeViewIds.push_back(referenceView->view.id);
    }
    const bool hasNewLineage =
        target.source == CropHostSource::CurrentImage
        && m_lastImage
        && (m_lastImage->version != input->inputVersion
            || m_lastImage->image.GetPointer()
                != input->imageData.GetPointer());
    const bool isStarted = m_bridge->StartView(
        request, std::move(*input));
    if (isStarted) {
        if (!SetActiveViews(activeViewIds)) {
            (void)m_bridge->ExitCrop();
            (void)m_bridge->ClearBindings();
            return false;
        }
        if (hasNewLineage) {
            m_rootImage.reset();
            m_lastImage.reset();
        }
        m_activeTarget = target;
        m_activeViewIds = std::move(activeViewIds);
        SendStatus();
    }
    return isStarted;
}

bool CropHostFeature::Impl::SetPolyData(
    vtkSmartPointer<vtkPolyData> polyData,
    const std::uint64_t sourceVersion)
{
    if (!polyData
        || sourceVersion == 0
        || (m_sourceVersion != 0
            && sourceVersion <= m_sourceVersion)
        || polyData.GetPointer()
            == m_polyData.GetPointer()) {
        return false;
    }
    if (m_bridge) {
        (void)m_bridge->ClearBindings();
    }
    if (!SetActiveViews({})) {
        return false;
    }
    m_polyData = std::move(polyData);
    m_sourceVersion = sourceVersion;
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_status.reset();
    return true;
}

bool CropHostFeature::Impl::ClearPolyData()
{
    if (m_bridge) {
        (void)m_bridge->ClearBindings();
    }
    if (!SetActiveViews({})) {
        return false;
    }
    m_polyData = nullptr;
    m_sourceVersion = 0;
    m_activeTarget.reset();
    m_activeViewIds.clear();
    m_status.reset();
    return true;
}

bool CropHostFeature::Impl::SetImageResult(
    const TrustedImageSnapshot& sourceSnapshot,
    const TrustedImageSnapshot& expectedSnapshot,
    CropBuildResult& result)
{
    if (!result.isSucceeded
        || result.resolvedDataSource
            != OrthogonalCropDataSource::ImageData
        || !result.imageData
        || !result.maskImage
        || !sourceSnapshot
        || !expectedSnapshot
        || !m_data
        || m_isPublishing) {
        std::cout
            << "[Crop][Materialize] reject invalid result or host state"
            << " succeeded=" << result.isSucceeded
            << " source="
            << static_cast<int>(result.resolvedDataSource)
            << " hasImage=" << static_cast<bool>(result.imageData)
            << " hasMask=" << static_cast<bool>(result.maskImage)
            << " hasSource=" << static_cast<bool>(sourceSnapshot)
            << " hasExpected=" << static_cast<bool>(expectedSnapshot)
            << " isPublishing=" << m_isPublishing << '\n';
        return false;
    }

    const auto historyBefore =
        m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] result"
        << " resultVersion=" << result.inputVersion
        << " sourceVersion=" << sourceSnapshot->version
        << " expectedVersion=" << expectedSnapshot->version
        << " prefixNodes=" << result.nodeCount
        << " prefixOps=" << result.operations.size()
        << " | " << GetHistoryText(historyBefore)
        << '\n';
    const std::size_t absoluteNodeCount =
        historyBefore.baseNodeCount
        + historyBefore.nodeCount;
    if (result.inputVersion != sourceSnapshot->version
        || result.nodeCount != absoluteNodeCount
        || result.operations.size() != result.nodeCount) {
        result.isSucceeded = false;
        result.failureReason =
            CropFailure::VersionMismatch;
        result.message =
            "Crop history changed while materialization was running.";
        result.imageData = nullptr;
        result.maskImage = nullptr;
        std::cout
            << "[Crop][Materialize] reject snapshot/history mismatch"
            << " | " << GetHistoryText(historyBefore)
            << '\n';
        return false;
    }

    CropInputSnapshot input;
    input.dataSource =
        OrthogonalCropDataSource::ImageData;
    input.inputVersion =
        expectedSnapshot->version + 1;
    input.imageData = result.imageData;
    input.validityMask = result.maskImage;
    input.imageData->GetBounds(
        input.inputModelBounds.data());
    auto preparedCommit = m_bridge->BuildCropCommit(
        std::move(input), absoluteNodeCount);
    if (!preparedCommit) {
        result.isSucceeded = false;
        result.failureReason = CropFailure::BadInput;
        result.message =
            "Crop history could not prepare the materialized baseline.";
        std::cout
            << "[Crop][Materialize] reject commit preparation"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }
    std::cout << "[Crop][Materialize] baseline staged"
        << " nextVersion=" << expectedSnapshot->version + 1
        << " nextBase="
        << absoluteNodeCount
        << " nextActiveOps="
        << historyBefore.operationCount
            - historyBefore.nodeCount
        << '\n';

    const PublishGuard publishGuard(m_isPublishing);
    TrustedImageState candidate = *expectedSnapshot;
    candidate.image = result.imageData;
    candidate.validityMask = result.maskImage;
    bool isPublished = false;
    TrustedImageSnapshot publishedSnapshot;
    try {
        isPublished = m_data->SetImageState(
            std::move(candidate),
            expectedSnapshot,
            publishedSnapshot);
    }
    catch (...) {
        isPublished = false;
    }
    if (!isPublished) {
        result.isSucceeded = false;
        result.failureReason =
            CropFailure::VersionMismatch;
        result.message =
            "Crop image snapshot changed before materialization.";
        result.imageData = nullptr;
        result.maskImage = nullptr;
        std::cout
            << "[Crop][Materialize] CAS publish rejected"
            << " expectedVersion=" << expectedSnapshot->version
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }

    // TrustedFeatureDataPort 保证 true 与同一 published owner 不可分割；
    // 从这里开始只接管 move-only 准备令牌，禁止再把已发布事务降级成普通失败。
    m_bridge->SetCropCommit(std::move(*preparedCommit));

    if (!m_rootImage) {
        m_rootImage = sourceSnapshot;
        std::cout
            << "[Crop][Materialize] root captured"
            << " rootVersion=" << m_rootImage->version
            << " rootImage="
            << static_cast<const void*>(
                m_rootImage->image.GetPointer())
            << " rootMask="
            << static_cast<const void*>(
                m_rootImage->validityMask.GetPointer())
            << '\n';
    }
    if (m_lastImage) {
        std::cout
            << "[Crop][Materialize] retire previous"
            << " version=" << m_lastImage->version
            << " image="
            << static_cast<const void*>(
                m_lastImage->image.GetPointer())
            << " mask="
            << static_cast<const void*>(
                m_lastImage->validityMask.GetPointer())
            << '\n';
        m_lastImage.reset();
    }
    m_lastImage = std::move(publishedSnapshot);
    (void)m_bridge->SendCropCommit();
    const auto historyAfter =
        m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] baseline complete"
        << " publishedVersion=" << m_lastImage->version
        << " image="
        << static_cast<const void*>(
            m_lastImage->image.GetPointer())
        << " mask="
        << static_cast<const void*>(
            m_lastImage->validityMask.GetPointer())
        << " | " << GetHistoryText(historyAfter)
        << '\n';
    // 重基准不会产生新的 shader revision，因此不能依赖 shader tick 间接刷新
    // 标题；此处直接发布 active node 0 和两条历史链的最新状态。
    SendStatus();
    return true;
}

bool CropHostFeature::Impl::ResetOriginal()
{
    if (!m_rootImage
        || !m_bridge
        || !m_data
        || m_isPublishing) {
        std::cout
            << "[Crop][History] reject root baseline"
            << " hasRoot=" << static_cast<bool>(m_rootImage)
            << " hasBridge=" << static_cast<bool>(m_bridge)
            << " hasPublisher="
            << static_cast<bool>(m_data)
            << " hasSnapshotReader="
            << static_cast<bool>(m_data)
            << " isPublishing=" << m_isPublishing
            << '\n';
        return false;
    }
    const auto expectedSnapshot =
        m_data->GetImageSnapshot();
    if (!expectedSnapshot
        || (m_lastImage
            && expectedSnapshot != m_lastImage)) {
        std::cout
            << "[Crop][History] reject root snapshot mismatch"
            << " hasExpected="
            << static_cast<bool>(expectedSnapshot)
            << " hasLast=" << static_cast<bool>(m_lastImage)
            << " expectedVersion="
            << (expectedSnapshot
                ? expectedSnapshot->version : 0)
            << " lastVersion="
            << (m_lastImage ? m_lastImage->version : 0)
            << '\n';
        return false;
    }
    const auto history = m_bridge->GetCropHistory();
    std::cout << "[Crop][History] explicit root restore"
        << " expectedVersion=" << expectedSnapshot->version
        << " currentImage="
        << static_cast<const void*>(
            expectedSnapshot->image.GetPointer())
        << " rootVersion=" << m_rootImage->version
        << " rootImage="
        << static_cast<const void*>(
            m_rootImage->image.GetPointer())
        << " rootMask="
        << static_cast<const void*>(
            m_rootImage->validityMask.GetPointer())
        << " | " << GetHistoryText(history)
        << '\n';

    CropInputSnapshot input;
    input.dataSource =
        OrthogonalCropDataSource::ImageData;
    input.inputVersion =
        expectedSnapshot->version + 1;
    input.imageData = m_rootImage->image;
    input.validityMask = m_rootImage->validityMask;
    input.imageData->GetBounds(
        input.inputModelBounds.data());
    auto preparedCommit = m_bridge->BuildCropCommit(
        std::move(input), 0);
    if (!preparedCommit) {
        std::cout
            << "[Crop][History] reject root commit preparation"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }

    const PublishGuard publishGuard(m_isPublishing);
    TrustedImageState candidate = *m_rootImage;
    bool isPublished = false;
    TrustedImageSnapshot publishedSnapshot;
    try {
        isPublished = m_data->SetImageState(
            std::move(candidate),
            expectedSnapshot,
            publishedSnapshot);
    }
    catch (...) {
        isPublished = false;
    }
    if (!isPublished) {
        std::cout
            << "[Crop][History] root CAS publish rejected"
            << " expectedVersion=" << expectedSnapshot->version
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }
    m_bridge->SetCropCommit(std::move(*preparedCommit));
    if (m_lastImage) {
        std::cout
            << "[Crop][History] retire materialized"
            << " version=" << m_lastImage->version
            << " image="
            << static_cast<const void*>(
                m_lastImage->image.GetPointer())
            << " mask="
            << static_cast<const void*>(
                m_lastImage->validityMask.GetPointer())
            << '\n';
        m_lastImage.reset();
    }
    m_lastImage = std::move(publishedSnapshot);
    (void)m_bridge->SendCropCommit();
    std::cout << "[Crop][History] root baseline complete"
        << " publishedVersion=" << m_lastImage->version
        << " publishedImage="
        << static_cast<const void*>(
            m_lastImage->image.GetPointer())
        << " publishedMask="
        << static_cast<const void*>(
            m_lastImage->validityMask.GetPointer())
        << " | " << GetHistoryText(
            m_bridge->GetCropHistory())
        << '\n';
    SendStatus();
    return true;
}

bool CropHostFeature::Impl::RemoveComplete(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item)
{
    if (!state || !item) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(state->mutex);
    const auto current = std::find(
        state->items.begin(), state->items.end(), item);
    if (current == state->items.end()) {
        return false;
    }
    state->items.erase(current);
    return true;
}

bool CropHostFeature::Impl::SetCompleteResult(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item,
    CropBuildResult result,
    std::optional<RenderInputStamp> waitInput)
{
    if (!state || !item) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->isActive
        || std::find(
            state->items.begin(),
            state->items.end(),
            item) == state->items.end()) {
        return false;
    }
    item->result = std::move(result);
    item->waitInput = waitInput;
    return true;
}

bool CropHostFeature::Impl::SendComplete(
    const std::shared_ptr<CompleteState>& state,
    const std::shared_ptr<CompleteItem>& item)
{
    if (!state || !item) {
        return false;
    }

    CropBuildCallback onComplete;
    CropBuildResult result;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        const auto current = std::find(
            state->items.begin(), state->items.end(), item);
        if (!state->isActive
            || current == state->items.end()
            || !item->onComplete
            || !item->result) {
            return false;
        }
        onComplete = std::move(item->onComplete);
        result = std::move(item->result.value());
        state->items.erase(current);
    }
    onComplete(std::move(result));
    return true;
}

bool CropHostFeature::Impl::SendReadyCompletes()
{
    if (!m_completeState
        || !m_host
        || !m_views) {
        return false;
    }

    std::vector<std::shared_ptr<CompleteItem>> readyItems;
    {
        const std::lock_guard<std::mutex> lock(
            m_completeState->mutex);
        if (!m_completeState->isActive) {
            return false;
        }
        for (const auto& item : m_completeState->items) {
            if (!item
                || !item->result
                || item->isQueued) {
                continue;
            }
            bool isReady = !item->waitInput.has_value();
            if (item->waitInput) {
                HostViewTargets targets;
                targets.viewIds = m_activeViewIds;
                const auto views = m_views->GetViews(targets);
                isReady = !views.empty()
                    && std::all_of(
                        views.begin(),
                        views.end(),
                        [this, &item](const auto& view) {
                            const auto port =
                                m_views->GetFeaturePort(view.id);
                            if (!port) {
                                return false;
                            }
                            const auto stamp =
                                port->GetRenderInputStamp();
                            return stamp
                                && *stamp == *item->waitInput;
                        });
            }
            if (isReady) {
                item->isQueued = true;
                readyItems.push_back(item);
            }
        }
    }

    const auto state = m_completeState;
    bool isSent = false;
    for (const auto& item : readyItems) {
        if (item->waitInput) {
            std::cout
                << "[Crop][Materialize] render convergence complete"
                << " version=" << item->waitInput->version
                << " image="
                << static_cast<const void*>(
                    item->waitInput->identity)
                << '\n';
        }
        const std::weak_ptr<CompleteState> weakState =
            state;
        const std::weak_ptr<CompleteItem> weakItem =
            item;
        if (m_host->SendOwnerComplete(
                [weakState, weakItem]() {
                    (void)Impl::SendComplete(
                        weakState.lock(),
                        weakItem.lock());
                })) {
            isSent = true;
        }
        else {
            (void)RemoveComplete(state, item);
        }
    }
    return isSent;
}

bool CropHostFeature::Impl::BuildCropResult(
    const CropHostTarget& target,
    CropBuildCallback onComplete)
{
    if (!onComplete
        || !m_bridge
        || !SetCropInput(target)
        || !m_completeState
        || !m_host) {
        return false;
    }

    const auto expectedSnapshot =
        target.source == CropHostSource::CurrentImage
        && m_data
        ? m_data->GetImageSnapshot()
        : TrustedImageSnapshot{};
    if (target.source == CropHostSource::CurrentImage
        && !expectedSnapshot) {
        return false;
    }
    const TrustedImageSnapshot sourceSnapshot =
        target.source == CropHostSource::CurrentImage
        ? (m_rootImage
            ? m_rootImage
            : expectedSnapshot)
        : TrustedImageSnapshot{};
    CropInputSnapshot rootInput;
    if (sourceSnapshot) {
        rootInput.dataSource =
            OrthogonalCropDataSource::ImageData;
        rootInput.inputVersion =
            sourceSnapshot->version;
        rootInput.imageData =
            sourceSnapshot->image;
        rootInput.validityMask =
            sourceSnapshot->validityMask;
        if (!GetImageReady(rootInput.imageData)) {
            return false;
        }
        rootInput.imageData->GetBounds(
            rootInput.inputModelBounds.data());
    }
    else {
        auto polyRoot = GetCropInput(target);
        if (!polyRoot) {
            return false;
        }
        rootInput = std::move(*polyRoot);
    }
    const auto history = m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] request"
        << " expectedVersion="
        << (expectedSnapshot
            ? expectedSnapshot->version : 0)
        << " sourceVersion="
        << (sourceSnapshot
            ? sourceSnapshot->version : 0)
        << " | " << GetHistoryText(history)
        << '\n';
    const auto state = m_completeState;
    const auto item = std::make_shared<CompleteItem>();
    item->onComplete = std::move(onComplete);
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->isActive) {
            return false;
        }
        state->items.push_back(item);
    }

    const std::weak_ptr<CompleteState> weakState = state;
    const std::weak_ptr<CompleteItem> weakItem = item;
    auto onResult =
        [this, sourceSnapshot, expectedSnapshot,
            weakState, weakItem](
            CropBuildResult result) mutable {
            const auto state = weakState.lock();
            const auto item = weakItem.lock();
            std::optional<RenderInputStamp> waitInput;
            if (result.isSucceeded
                && result.resolvedDataSource
                    == OrthogonalCropDataSource::ImageData) {
                if (SetImageResult(
                        sourceSnapshot,
                        expectedSnapshot, result)) {
                    waitInput = RenderInputStamp{
                        result.imageData.GetPointer(),
                        expectedSnapshot->version + 1
                    };
                    std::cout
                        << "[Crop][Materialize] waiting render convergence"
                        << " version=" << waitInput->version
                        << " image="
                        << static_cast<const void*>(
                            waitInput->identity)
                        << '\n';
                }
            }
            if (!Impl::SetCompleteResult(
                    state,
                    item,
                    std::move(result),
                    waitInput)) {
                return;
            }
        };
    const bool isAccepted = m_bridge->BuildCropResult(
        std::move(rootInput),
        std::move(onResult));
    if (!isAccepted) {
        // Bridge 可以在同步校验失败时先回传内部结果；入口返回 false 时必须丢弃，
        // 保证未接纳请求的外部 callback 永远不会在后续 tick 泄漏。
        (void)RemoveComplete(state, item);
    }
    return isAccepted;
}

bool CropHostFeature::Impl::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    if (!m_isAttached
        || m_ownerThread != std::this_thread::get_id()
        || m_isPublishing
        || ((request.action == CropHostAction::BuildResult)
            != static_cast<bool>(onComplete))) {
        return false;
    }
    switch (request.action) {
    case CropHostAction::Start:
        if (request.target) {
            return SendActionLog(
                "Start", StartCrop(*request.target));
        }
        return false;
    case CropHostAction::Box:
        if (request.target) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SwitchCropBox();
            return SendActionLog(
                "SwitchBox", isAccepted);
        }
        return false;
    case CropHostAction::Plane:
        if (request.target) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SwitchCropPlane();
            return SendActionLog(
                "SwitchPlane", isAccepted);
        }
        return false;
    case CropHostAction::Mode:
        if (request.target && request.removalMode) {
            const bool isAccepted =
                StartCrop(*request.target)
                && m_bridge->SetCropMode(
                    *request.removalMode);
            return SendActionLog(
                GetModeText(*request.removalMode),
                isAccepted);
        }
        return false;
    case CropHostAction::Previous:
        return SendActionLog(
            "Previous",
            m_activeTarget.has_value()
            && m_bridge->GetCropBound()
            && m_bridge->PreviousCrop());
    case CropHostAction::Next:
        return SendActionLog(
            "Next",
            m_activeTarget.has_value()
            && m_bridge->GetCropBound()
            && m_bridge->NextCrop());
    case CropHostAction::Node:
        if (request.nodeCount) {
            const bool isAccepted =
                m_activeTarget.has_value()
                && m_bridge->GetCropBound()
                && m_bridge->SetCropNode(
                    *request.nodeCount);
            std::cout
                << "[Crop][Request] targetNode="
                << *request.nodeCount << '\n';
            return SendActionLog(
                "SetNode", isAccepted);
        }
        return false;
    case CropHostAction::BuildResult:
        if (request.target) {
            return BuildCropResult(
                *request.target, std::move(onComplete));
        }
        return false;
    case CropHostAction::SetPolyData:
        if (request.polyData && request.sourceVersion) {
            return SetPolyData(
                std::move(request.polyData),
                *request.sourceVersion);
        }
        return false;
    case CropHostAction::ClearPolyData:
        return ClearPolyData();
    case CropHostAction::RestoreOriginal:
        return SendActionLog(
            "RestoreOriginal",
            ResetOriginal());
    case CropHostAction::Exit:
        m_status.reset();
        {
            const bool isExited = m_bridge->ExitCrop();
            const bool isQualityRestored =
                !isExited || SetActiveViews({});
            return SendActionLog(
                "Exit", isExited && isQualityRestored);
        }
    case CropHostAction::None:
        return false;
    }
    return false;
}

CropHostState CropHostFeature::Impl::GetState() const
{
    CropHostState state;
    if (!m_isAttached
        || !m_bridge
        || m_ownerThread != std::this_thread::get_id()) {
        return state;
    }

    // owner thread 上连续读取 history、active 和发布屏障，形成同一时刻的只读快照。
    state.history = m_bridge->GetCropHistory();
    state.isActive = m_activeTarget.has_value()
        && m_bridge->GetCropActive();
    state.isPublishing = m_isPublishing;
    return state;
}

std::optional<std::size_t>
CropHostFeature::Impl::GetKeyIndex(
    const InteractionEvent& event) const
{
    const std::array<const HostKeyChord*,
        kCommandKeyCount> keys = {
        &m_config.keys.box,
        &m_config.keys.plane,
        &m_config.keys.noMode,
        &m_config.keys.keepMode,
        &m_config.keys.removeMode,
        &m_config.keys.previous,
        &m_config.keys.next,
        &m_config.keys.buildResult,
        &m_config.keys.restoreOriginal,
        &m_config.keys.exit
    };
    for (std::size_t index = 0;
        index < keys.size(); ++index) {
        if (GetChordMatched(event, *keys[index])) {
            return index;
        }
    }
    for (std::size_t node = 0;
        node < m_config.keys.nodes.size(); ++node) {
        if (GetChordMatched(
                event, m_config.keys.nodes[node])) {
            return keys.size() + node;
        }
    }
    return std::nullopt;
}

CropHostRequest CropHostFeature::Impl::GetKeyRequest(
    const std::size_t keyIndex) const
{
    CropHostRequest request;
    if (keyIndex == 0) {
        request.action = CropHostAction::Box;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex == 1) {
        request.action = CropHostAction::Plane;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex >= 2 && keyIndex <= 4) {
        CropRemovalMode mode = CropRemovalMode::None;
        if (keyIndex == 3) {
            mode = CropRemovalMode::KeepInside;
        }
        else if (keyIndex == 4) {
            mode = CropRemovalMode::RemoveInside;
        }
        request.action = CropHostAction::Mode;
        request.target = m_config.defaultTarget;
        request.removalMode = mode;
    }
    else if (keyIndex == 5) {
        request.action = CropHostAction::Previous;
    }
    else if (keyIndex == 6) {
        request.action = CropHostAction::Next;
    }
    else if (keyIndex == 7) {
        request.action = CropHostAction::BuildResult;
        request.target = m_config.defaultTarget;
    }
    else if (keyIndex == 8) {
        request.action = CropHostAction::RestoreOriginal;
    }
    else if (keyIndex == 9) {
        request.action = CropHostAction::Exit;
    }
    else {
        request.action = CropHostAction::Node;
        request.nodeCount = keyIndex - kCommandKeyCount;
    }
    return request;
}

InteractionResult CropHostFeature::Impl::OnInput(
    const InteractionEvent& event)
{
    const auto keyIndex = GetKeyIndex(event);
    if (!keyIndex || *keyIndex >= m_isDown.size()) {
        return {};
    }
    if (event.eventKind
        == InteractionEventKind::KeyRelease) {
        m_isDown[*keyIndex] = false;
        return { true, true };
    }
    if (event.eventKind
        == InteractionEventKind::TextInput) {
        return { true, true };
    }
    if (event.eventKind
        != InteractionEventKind::KeyPress) {
        return {};
    }
    if (m_isDown[*keyIndex]) {
        return { true, true };
    }
    m_isDown[*keyIndex] = true;
    auto request = GetKeyRequest(*keyIndex);
    CropBuildCallback callback;
    if (request.action == CropHostAction::BuildResult) {
        callback = [](CropBuildResult result) {
            std::cout
                << "[Crop][Materialize] callback"
                << " succeeded=" << result.isSucceeded
                << " failure="
                << static_cast<int>(result.failureReason)
                << " inputVersion=" << result.inputVersion
                << " materializedPrefix="
                << result.nodeCount
                << " operationCount="
                << result.operations.size()
                << " message=\"" << result.message
                << "\"\n";
        };
    }
    const auto action = request.action;
    const bool isAccepted = SendRequest(
        std::move(request), std::move(callback));
    std::cout
        << "[Crop][Hotkey] action="
        << GetActionText(action)
        << " keyCode="
        << static_cast<int>(event.keyCode)
        << " keySym=\"" << event.keySym << '"'
        << " ctrl=" << event.isCtrlDown
        << " alt=" << event.isAltDown
        << " shift=" << event.isShiftDown
        << " accepted=" << isAccepted
        << '\n';
    return { true, true };
}

bool CropHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached
        || !m_bridge
        || m_ownerThread != std::this_thread::get_id()
        || m_isPublishing) {
        return false;
    }
    if (m_bridge->GetCropBound()
        && m_activeTarget
        && !SetCropInput(*m_activeTarget)) {
        (void)m_bridge->ClearBindings();
        (void)SetActiveViews({});
        m_activeTarget.reset();
        m_activeViewIds.clear();
        m_status.reset();
    }
    if (m_bridge->GetShaderTickNeeded()
        && m_bridge->SendShaderCommit()) {
        std::cout
            << "[Crop][History] shader commit"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        SendStatus();
    }
    if (m_bridge->GetBuildTickNeeded()) {
        (void)m_bridge->SendBuildResult();
    }
    (void)SendReadyCompletes();
    return true;
}

bool CropHostFeature::Impl::SendActionLog(
    const char* action,
    const bool isAccepted) const
{
    std::cout
        << "[Crop][Request] action="
        << (action ? action : "Unknown")
        << " accepted=" << isAccepted;
    if (m_bridge) {
        std::cout << " | " << GetHistoryText(
            m_bridge->GetCropHistory());
    }
    std::cout << '\n';
    return isAccepted;
}

const char* CropHostFeature::Impl::GetActionText(
    const CropHostAction action)
{
    switch (action) {
    case CropHostAction::Start: return "Start";
    case CropHostAction::Box: return "Box";
    case CropHostAction::Plane: return "Plane";
    case CropHostAction::Mode: return "Mode";
    case CropHostAction::Previous: return "Previous";
    case CropHostAction::Next: return "Next";
    case CropHostAction::Node: return "Node";
    case CropHostAction::BuildResult: return "BuildResult";
    case CropHostAction::SetPolyData: return "SetPolyData";
    case CropHostAction::ClearPolyData: return "ClearPolyData";
    case CropHostAction::RestoreOriginal:
        return "RestoreOriginal";
    case CropHostAction::Exit: return "Exit";
    case CropHostAction::None: return "None";
    }
    return "Unknown";
}

const char* CropHostFeature::Impl::GetModeText(
    const CropRemovalMode removalMode)
{
    switch (removalMode) {
    case CropRemovalMode::KeepInside:
        return "ModeKeepInside";
    case CropRemovalMode::RemoveInside:
        return "ModeRemoveInside";
    case CropRemovalMode::None:
        return "ModeNone";
    }
    return "ModeUnknown";
}

std::string CropHostFeature::Impl::GetHistoryText(
    const CropHistoryState& state)
{
    std::ostringstream text;
    text << "activeHistoryNode=" << state.nodeCount
        << " activeHistorySize=" << state.operationCount
        << " baseNode=" << state.baseNodeCount
        << " allHistorySize=" << state.allOperationCount;
    return text.str();
}

void CropHostFeature::Impl::SendStatus()
{
    if (!m_activeTarget
        || !m_activeTarget->isStatusVisible
        || !m_bridge
        || !m_host) {
        return;
    }
    const auto state = m_bridge->GetCropHistory();
    if (m_status
        && m_status->nodeCount == state.nodeCount
        && m_status->operationCount
            == state.operationCount
        && m_status->editMode == state.editMode
        && m_status->hasEditableOp
            == state.hasEditableOp
        && m_status->isEditing == state.isEditing
        && m_status->baseNodeCount
            == state.baseNodeCount
        && m_status->allOperationCount
            == state.allOperationCount) {
        return;
    }

    const char* stateText = "Idle";
    if (!state.isEditing && state.operationCount > 0) {
        stateText = "Frozen";
    }
    else if (state.editMode
        == CropRemovalMode::KeepInside) {
        stateText = "KeepInside";
    }
    else if (state.editMode
        == CropRemovalMode::RemoveInside) {
        stateText = "RemoveInside";
    }

    std::ostringstream status;
    status << "Crop active " << state.nodeCount << '/'
        << state.operationCount
        << " | base " << state.baseNodeCount
        << " | all " << state.allOperationCount
        << " | " << stateText;
    if (state.hasEditableOp) {
        status << " | Editable";
    }
    (void)m_host->SetViewStatus(m_activeViewIds, status.str());
    std::cout << "[Crop] " << status.str() << '\n';
    m_status = state;
}

CropHostFeature::CropHostFeature(CropHostConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

CropHostFeature::~CropHostFeature() noexcept = default;

std::string_view CropHostFeature::GetFeatureId() const noexcept
{
    return kFeatureId;
}

bool CropHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl && m_impl->AttachHost(*this, context);
}

bool CropHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool CropHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

bool CropHostFeature::SendRequest(
    CropHostRequest request,
    CropBuildCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request), std::move(onComplete));
}

CropHostState CropHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : CropHostState{};
}
