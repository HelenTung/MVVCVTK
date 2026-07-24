#include "Host/CropHostFeature.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "Host/HostRenderViewSet.h"
#include "Interaction/CropBridge.h"
#include "StdRenderContext.h"

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
        CropHostCallback onComplete;
        std::optional<CropExportResult> result;
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
        CropHostCallback onComplete);

private:
    bool StartCrop(const CropHostTarget& target);
    bool SetCropInput(const CropHostTarget& target);
    bool SetPolyData(CropHostPolyDataRequest request);
    bool ClearPolyData();
    bool ResetOriginal();
    bool SetImageResult(
        const ImageSnapshot& expectedSnapshot,
        CropExportResult& result);
    bool ExportCrop(
        const CropHostTarget& target,
        CropHostCallback onComplete);
    static bool RemoveComplete(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item);
    static bool SetCompleteResult(
        const std::shared_ptr<CompleteState>& state,
        const std::shared_ptr<CompleteItem>& item,
        CropExportResult result,
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
    const HostRenderViewSet* m_renderViews = nullptr;
    HostInputPort* m_inputPort = nullptr;
    std::function<ImageSnapshot()> m_getImageSnapshot;
    std::function<bool(
        ImageState,
        const ImageSnapshot&,
        ImageSnapshot&)> m_setImageState;
    std::function<bool(std::function<void()>)>
        m_sendOwnerComplete;
    vtkSmartPointer<vtkPolyData> m_polyData;
    std::uint64_t m_sourceVersion = 0;
    ImageSnapshot m_rootImage;
    ImageSnapshot m_lastImage;
    std::optional<CropHostTarget> m_activeTarget;
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
        return m_renderViews == context.renderViews
            && m_inputPort == context.inputPort
            && m_ownerThread == std::this_thread::get_id();
    }
    if (!context.renderViews
        || !context.inputPort
        || !context.getImageSnapshot
        || !context.setImageState
        || !context.sendOwnerComplete) {
        return false;
    }

    m_renderViews = context.renderViews;
    m_inputPort = context.inputPort;
    m_getImageSnapshot = context.getImageSnapshot;
    m_setImageState = context.setImageState;
    m_sendOwnerComplete = context.sendOwnerComplete;
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
        if (!m_inputPort->AttachInput(std::move(binding))) {
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

    if (m_inputPort
        && (!m_config.inputViews.viewIds.empty()
            || !m_config.inputViews.viewRoles.empty())) {
        if (!m_inputPort->DetachInput(kFeatureId)) {
            return false;
        }
    }
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
    m_status.reset();
    m_isDown.fill(false);
    m_isAttached = false;
    ClearBorrowed();
    return true;
}

void CropHostFeature::Impl::ClearBorrowed()
{
    m_renderViews = nullptr;
    m_inputPort = nullptr;
    m_getImageSnapshot = {};
    m_setImageState = {};
    m_sendOwnerComplete = {};
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
    if (!m_bridge) {
        return false;
    }

    CropInputSnapshot input;
    if (target.source == CropHostSource::CurrentImage) {
        const auto imageState = m_getImageSnapshot
            ? m_getImageSnapshot() : ImageSnapshot{};
        if (!imageState || !GetImageReady(imageState->image)) {
            return false;
        }
        if (m_lastImage && imageState != m_lastImage) {
            // File/Reload 或其它 owner 事务已替换数据，旧根快照不再属于当前 lineage。
            m_rootImage.reset();
            m_lastImage.reset();
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
            return false;
        }
        input.dataSource =
            OrthogonalCropDataSource::PolyData;
        input.inputVersion = m_sourceVersion;
        input.polyData = m_polyData;
        input.polyData->GetBounds(
            input.inputModelBounds.data());
    }
    return m_bridge->SetCropInput(std::move(input));
}

bool CropHostFeature::Impl::StartCrop(
    const CropHostTarget& target)
{
    if (!m_isAttached
        || !m_renderViews
        || !m_bridge
        || (target.referenceView.viewId.empty()
            && !target.referenceView.isViewRoleUsed)) {
        return false;
    }
    const auto* referenceView =
        m_renderViews->GetViewBySelector(
            target.referenceView);
    if (!referenceView
        || !referenceView->service
        || !referenceView->context) {
        return false;
    }

    auto targetViews = m_renderViews->GetViewsByTargets(
        target.targetViews);
    if (targetViews.empty() && !target.isTargetViewsUsed) {
        targetViews = m_renderViews->GetViewsByTargets(
            m_config.defaultTarget.targetViews);
    }

    CropViewRequest request;
    request.interactor =
        referenceView->context->GetInteractor();
    request.renderer =
        referenceView->context->GetRenderer();
    request.referenceService = referenceView->service;
    request.targetServices =
        m_renderViews->BuildServices(targetViews);
    const bool isStarted = SetCropInput(target)
        && m_bridge->StartView(request);
    if (isStarted) {
        m_activeTarget = target;
        SendStatus();
    }
    return isStarted;
}

bool CropHostFeature::Impl::SetPolyData(
    CropHostPolyDataRequest request)
{
    if (!request.polyData
        || request.sourceVersion == 0
        || (m_sourceVersion != 0
            && request.sourceVersion <= m_sourceVersion)
        || request.polyData.GetPointer()
            == m_polyData.GetPointer()) {
        return false;
    }
    if (m_bridge) {
        (void)m_bridge->ClearBindings();
    }
    m_polyData = std::move(request.polyData);
    m_sourceVersion = request.sourceVersion;
    m_activeTarget.reset();
    m_status.reset();
    return true;
}

bool CropHostFeature::Impl::ClearPolyData()
{
    if (m_bridge) {
        (void)m_bridge->ClearBindings();
    }
    m_polyData = nullptr;
    m_sourceVersion = 0;
    m_activeTarget.reset();
    m_status.reset();
    return true;
}

bool CropHostFeature::Impl::SetImageResult(
    const ImageSnapshot& expectedSnapshot,
    CropExportResult& result)
{
    if (!result.isSucceeded
        || result.resolvedDataSource
            != OrthogonalCropDataSource::ImageData
        || !result.imageData
        || !result.maskImage
        || !expectedSnapshot
        || !m_setImageState
        || !m_getImageSnapshot
        || m_isPublishing) {
        std::cout
            << "[Crop][Materialize] reject invalid result or host state"
            << " succeeded=" << result.isSucceeded
            << " source="
            << static_cast<int>(result.resolvedDataSource)
            << " hasImage=" << static_cast<bool>(result.imageData)
            << " hasMask=" << static_cast<bool>(result.maskImage)
            << " hasExpected=" << static_cast<bool>(expectedSnapshot)
            << " isPublishing=" << m_isPublishing << '\n';
        return false;
    }

    const auto historyBefore =
        m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] result"
        << " resultVersion=" << result.inputVersion
        << " expectedVersion=" << expectedSnapshot->version
        << " prefixNodes=" << result.nodeCount
        << " prefixOps=" << result.operations.size()
        << " | " << GetHistoryText(historyBefore)
        << '\n';
    if (result.inputVersion != expectedSnapshot->version
        || result.nodeCount != historyBefore.nodeCount
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
    if (!m_bridge->StartCropBaseline(
            std::move(input),
            historyBefore.baseNodeCount
                + historyBefore.nodeCount)) {
        result.isSucceeded = false;
        result.failureReason = CropFailure::BadInput;
        result.message =
            "Crop history could not stage the materialized baseline.";
        std::cout
            << "[Crop][Materialize] reject baseline staging"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }
    std::cout << "[Crop][Materialize] baseline staged"
        << " nextVersion=" << expectedSnapshot->version + 1
        << " nextBase="
        << historyBefore.baseNodeCount
            + historyBefore.nodeCount
        << " nextActiveOps="
        << historyBefore.operationCount
            - historyBefore.nodeCount
        << '\n';

    const PublishGuard publishGuard(m_isPublishing);
    ImageState candidate = *expectedSnapshot;
    candidate.image = result.imageData;
    candidate.validityMask = result.maskImage;
    bool isPublished = false;
    ImageSnapshot publishedSnapshot;
    try {
        isPublished = m_setImageState(
            std::move(candidate),
            expectedSnapshot,
            publishedSnapshot);
    }
    catch (...) {
        isPublished = false;
    }
    if (!isPublished) {
        (void)m_bridge->ClearCropBaseline();
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

    if (!publishedSnapshot
        || publishedSnapshot->version
            != expectedSnapshot->version + 1
        || publishedSnapshot->image.GetPointer()
            != result.imageData.GetPointer()
        || publishedSnapshot->validityMask.GetPointer()
            != result.maskImage.GetPointer()) {
        // setImageState=true 必须携带同一 CAS 临界区内返回的发布令牌。
        // 违反该宿主契约时不能再按普通失败取消 baseline。
        std::terminate();
    }

    // StartCropBaseline 已完成全部可失败校验；CAS 发布后只执行无失败状态交换，
    // 避免新 image+mask 与旧 history/shader 基线分裂。
    (void)m_bridge->SetCropBaselineComplete();

    if (!m_rootImage) {
        m_rootImage = expectedSnapshot;
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
        || !m_setImageState
        || !m_getImageSnapshot
        || m_isPublishing) {
        std::cout
            << "[Crop][History] reject root baseline"
            << " hasRoot=" << static_cast<bool>(m_rootImage)
            << " hasBridge=" << static_cast<bool>(m_bridge)
            << " hasPublisher="
            << static_cast<bool>(m_setImageState)
            << " hasSnapshotReader="
            << static_cast<bool>(m_getImageSnapshot)
            << " isPublishing=" << m_isPublishing
            << '\n';
        return false;
    }
    const auto expectedSnapshot =
        m_getImageSnapshot();
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
    if (!m_bridge->StartCropBaseline(
            std::move(input), 0)) {
        std::cout
            << "[Crop][History] reject root baseline staging"
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }

    const PublishGuard publishGuard(m_isPublishing);
    ImageState candidate = *m_rootImage;
    bool isPublished = false;
    ImageSnapshot publishedSnapshot;
    try {
        isPublished = m_setImageState(
            std::move(candidate),
            expectedSnapshot,
            publishedSnapshot);
    }
    catch (...) {
        isPublished = false;
    }
    if (!isPublished) {
        (void)m_bridge->ClearCropBaseline();
        std::cout
            << "[Crop][History] root CAS publish rejected"
            << " expectedVersion=" << expectedSnapshot->version
            << " | " << GetHistoryText(
                m_bridge->GetCropHistory())
            << '\n';
        return false;
    }
    if (!publishedSnapshot
        || publishedSnapshot->version
            != expectedSnapshot->version + 1
        || publishedSnapshot->image.GetPointer()
            != m_rootImage->image.GetPointer()
        || publishedSnapshot->validityMask.GetPointer()
            != m_rootImage->validityMask.GetPointer()) {
        // setImageState=true 后只能使用原子发布令牌完成事务，不能二次读取并回退。
        std::terminate();
    }

    // baseNode=0 与普通物化复用同一个无失败完成阶段。
    (void)m_bridge->SetCropBaselineComplete();
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
    CropExportResult result,
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

    CropHostCallback onComplete;
    std::optional<CropExportResult> result;
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
        result = std::move(item->result);
        state->items.erase(current);
    }
    onComplete(std::move(*result));
    return true;
}

bool CropHostFeature::Impl::SendReadyCompletes()
{
    if (!m_completeState
        || !m_sendOwnerComplete
        || !m_renderViews) {
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
                const auto& views =
                    m_renderViews->GetViews();
                isReady = !views.empty()
                    && std::all_of(
                        views.begin(),
                        views.end(),
                        [&item](const auto& view) {
                            return view.service
                                && view.service
                                    ->GetRenderInputStamp()
                                    == *item->waitInput;
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
        if (m_sendOwnerComplete(
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

bool CropHostFeature::Impl::ExportCrop(
    const CropHostTarget& target,
    CropHostCallback onComplete)
{
    if (!onComplete
        || !m_bridge
        || !SetCropInput(target)
        || !m_completeState
        || !m_sendOwnerComplete) {
        return false;
    }

    const auto expectedSnapshot =
        target.source == CropHostSource::CurrentImage
        && m_getImageSnapshot
        ? m_getImageSnapshot()
        : ImageSnapshot{};
    if (target.source == CropHostSource::CurrentImage
        && !expectedSnapshot) {
        return false;
    }
    const auto history = m_bridge->GetCropHistory();
    std::cout << "[Crop][Materialize] request"
        << " expectedVersion="
        << (expectedSnapshot
            ? expectedSnapshot->version : 0)
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
    const bool isAccepted = m_bridge->ExportCrop(
        [this, expectedSnapshot,
            weakState, weakItem](
            CropExportResult result) mutable {
            const auto state = weakState.lock();
            const auto item = weakItem.lock();
            std::optional<RenderInputStamp> waitInput;
            if (result.isSucceeded
                && result.resolvedDataSource
                    == OrthogonalCropDataSource::ImageData) {
                if (SetImageResult(
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
        });
    if (!isAccepted) {
        bool hasResult = false;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            hasResult = item->result.has_value();
        }
        if (!hasResult) {
            (void)RemoveComplete(state, item);
        }
    }
    return isAccepted;
}

bool CropHostFeature::Impl::SendRequest(
    CropHostRequest request,
    CropHostCallback onComplete)
{
    if (!m_isAttached
        || m_ownerThread != std::this_thread::get_id()
        || m_isPublishing
        || (request.action != CropHostAction::Export
            && onComplete)) {
        return false;
    }
    switch (request.action) {
    case CropHostAction::Start:
        if (const auto* value =
            std::get_if<CropHostTarget>(&request.payload)) {
            return SendActionLog(
                "Start", StartCrop(*value));
        }
        return false;
    case CropHostAction::Box:
        if (const auto* value =
            std::get_if<CropHostTarget>(&request.payload)) {
            const bool isAccepted =
                StartCrop(*value)
                && m_bridge->SwitchCropBox();
            return SendActionLog(
                "SwitchBox", isAccepted);
        }
        return false;
    case CropHostAction::Plane:
        if (const auto* value =
            std::get_if<CropHostTarget>(&request.payload)) {
            const bool isAccepted =
                StartCrop(*value)
                && m_bridge->SwitchCropPlane();
            return SendActionLog(
                "SwitchPlane", isAccepted);
        }
        return false;
    case CropHostAction::Mode:
        if (const auto* value =
            std::get_if<CropHostModeRequest>(
                &request.payload)) {
            const bool isAccepted =
                StartCrop(value->target)
                && m_bridge->SetCropMode(
                    value->removalMode);
            return SendActionLog(
                GetModeText(value->removalMode),
                isAccepted);
        }
        return false;
    case CropHostAction::Previous:
        return SendActionLog(
            "Previous",
            std::holds_alternative<std::monostate>(
                request.payload)
            && m_bridge->PreviousCrop());
    case CropHostAction::Next:
        return SendActionLog(
            "Next",
            std::holds_alternative<std::monostate>(
                request.payload)
            && m_bridge->NextCrop());
    case CropHostAction::Node:
        if (const auto* value =
            std::get_if<CropHostNodeRequest>(
                &request.payload)) {
            const bool isAccepted =
                m_bridge->SetCropNode(
                    value->nodeCount);
            std::cout
                << "[Crop][Request] targetNode="
                << value->nodeCount << '\n';
            return SendActionLog(
                "SetNode", isAccepted);
        }
        return false;
    case CropHostAction::Export:
        if (const auto* value =
            std::get_if<CropHostTarget>(&request.payload)) {
            return ExportCrop(
                *value, std::move(onComplete));
        }
        return false;
    case CropHostAction::SetPolyData:
        if (auto* value =
            std::get_if<CropHostPolyDataRequest>(
                &request.payload)) {
            return SetPolyData(std::move(*value));
        }
        return false;
    case CropHostAction::ClearPolyData:
        return std::holds_alternative<std::monostate>(
                request.payload)
            && ClearPolyData();
    case CropHostAction::RestoreOriginal:
        return SendActionLog(
            "RestoreOriginal",
            std::holds_alternative<std::monostate>(
                request.payload)
            && ResetOriginal());
    case CropHostAction::Exit:
        if (!std::holds_alternative<std::monostate>(
                request.payload)) {
            return false;
        }
        m_activeTarget.reset();
        m_status.reset();
        return SendActionLog(
            "Exit", m_bridge->ExitCrop());
    case CropHostAction::None:
        return false;
    }
    return false;
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
        &m_config.keys.exportResult,
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
        request = { CropHostAction::Box,
            m_config.defaultTarget };
    }
    else if (keyIndex == 1) {
        request = { CropHostAction::Plane,
            m_config.defaultTarget };
    }
    else if (keyIndex >= 2 && keyIndex <= 4) {
        CropRemovalMode mode = CropRemovalMode::None;
        if (keyIndex == 3) {
            mode = CropRemovalMode::KeepInside;
        }
        else if (keyIndex == 4) {
            mode = CropRemovalMode::RemoveInside;
        }
        request = { CropHostAction::Mode,
            CropHostModeRequest{
                m_config.defaultTarget, mode } };
    }
    else if (keyIndex == 5) {
        request = { CropHostAction::Previous,
            std::monostate{} };
    }
    else if (keyIndex == 6) {
        request = { CropHostAction::Next,
            std::monostate{} };
    }
    else if (keyIndex == 7) {
        request = { CropHostAction::Export,
            m_config.defaultTarget };
    }
    else if (keyIndex == 8) {
        request = { CropHostAction::RestoreOriginal,
            std::monostate{} };
    }
    else if (keyIndex == 9) {
        request = { CropHostAction::Exit,
            std::monostate{} };
    }
    else {
        request = { CropHostAction::Node,
            CropHostNodeRequest{
                keyIndex - kCommandKeyCount } };
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
    CropHostCallback callback;
    if (request.action == CropHostAction::Export) {
        callback = [](CropExportResult result) {
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
    if (m_bridge->GetCropActive()
        && m_activeTarget
        && !SetCropInput(*m_activeTarget)) {
        (void)m_bridge->ClearBindings();
        m_activeTarget.reset();
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
    if (m_bridge->GetExportTickNeeded()) {
        (void)m_bridge->SendExportResult();
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
    case CropHostAction::Export: return "Export";
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
    if (!m_renderViews
        || !m_activeTarget
        || !m_activeTarget->isStatusVisible
        || !m_bridge) {
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
    auto views = m_renderViews->GetViewsByTargets(
        m_activeTarget->targetViews);
    if (views.empty()
        && !m_activeTarget->isTargetViewsUsed) {
        views = m_renderViews->GetViewsByTargets(
            m_config.defaultTarget.targetViews);
    }
    if (views.empty()) {
        if (const auto* reference =
            m_renderViews->GetViewBySelector(
                m_activeTarget->referenceView)) {
            views.push_back(reference);
        }
    }
    for (const auto* view : views) {
        if (!view || !view->context) {
            continue;
        }
        std::string title = view->config.window.title;
        if (!title.empty()) {
            title += " | ";
        }
        title += status.str();
        view->context->SetWindowTitle(title);
    }
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
    CropHostCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request), std::move(onComplete));
}
