#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostRenderViewSet.h"

#include "AppState.h"
#include "DataManager.h"
#include "StdRenderContext.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

std::function<ImageSnapshot()>
HostCoreServices::GetImageReader() const
{
    const std::weak_ptr<RawVolumeDataManager> weakData =
        sharedDataMgr;
    return [weakData]() {
        const auto data = weakData.lock();
        return data
            ? data->GetImageSnapshot()
            : ImageSnapshot{};
    };
}

std::function<bool(
    ImageState,
    const ImageSnapshot&,
    ImageSnapshot&)>
HostCoreServices::GetImageWriter() const
{
    const std::weak_ptr<RawVolumeDataManager> weakData =
        sharedDataMgr;
    const std::weak_ptr<SharedInteractionState> weakState =
        sharedState;
    return [weakData, weakState](
        ImageState state,
        const ImageSnapshot& expectedSnapshot,
        ImageSnapshot& publishedSnapshot) {
        publishedSnapshot.reset();
        const auto data = weakData.lock();
        const auto sharedState = weakState.lock();
        if (!data
            || !sharedState
            || !expectedSnapshot
            || !state.image
            || !data->SetCurrentData(
                std::move(state),
                expectedSnapshot,
                publishedSnapshot)) {
            return false;
        }

        // current 已发布；观察者异常不能把成功的 CAS 事务改报为失败。
        try {
            (void)sharedState->SetImageDataReady(
                publishedSnapshot->scalarRange[0],
                publishedSnapshot->scalarRange[1],
                publishedSnapshot->spacing);
        }
        catch (...) {
        }
        return true;
    };
}

class VtkAppHostSession::Impl final {
public:
    struct FeatureEntry final {
        std::string id;
        std::weak_ptr<HostFeature> feature;
    };

    struct OwnerCompleteState final {
        std::mutex mutex;
        std::vector<std::function<void()>> completes;
        bool isActive = true;
    };

    explicit Impl(HostSessionConfig sessionConfig)
        : config(std::move(sessionConfig))
        , ownerCompleteState(
            std::make_shared<OwnerCompleteState>())
    {
    }

    ~Impl();

    bool BuildSession();
    bool SendCommand(HostCommand command);
    bool AttachTimer(const HostTimerConfig& timerConfig);
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    bool DetachFeature(const HostFeature& feature);

    HostSessionConfig config;
    HostCoreServices core;
    HostRenderViewSet renderViews;
    std::shared_ptr<HostCommandRouter> commandRouter;
    std::vector<HostRenderViewEndpoint> endpoints;
    std::unique_ptr<HostHotkeyRouter> hotkeyRouter;
    std::vector<FeatureEntry> features;
    std::weak_ptr<StdRenderContext> timerContext;
    std::shared_ptr<OwnerCompleteState> ownerCompleteState;
    std::thread::id ownerThread;
    bool isBuilt = false;
    bool isStarted = false;

private:
    static HostCoreServices BuildCore();
    void OnHostTimer();
    void DetachTimer();
    bool DetachFeatures();
};

HostCoreServices VtkAppHostSession::Impl::BuildCore()
{
    HostCoreServices value;
    value.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    value.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    value.sharedState =
        std::make_shared<SharedInteractionState>(
            value.sharedStateBroadcaster);
    return value;
}

VtkAppHostSession::Impl::~Impl()
{
    if (isBuilt
        && ownerThread != std::this_thread::get_id()) {
        std::terminate();
    }
    if (hotkeyRouter) {
        (void)hotkeyRouter->ClearHotkeys();
    }
    DetachTimer();
    if (!DetachFeatures()) {
        std::terminate();
    }
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        ownerCompleteState->isActive = false;
        ownerCompleteState->completes.clear();
    }
    hotkeyRouter.reset();
    commandRouter.reset();
}

bool VtkAppHostSession::Impl::BuildSession()
{
    if (isBuilt) {
        return true;
    }
    if (config.renderViews.empty()) {
        return false;
    }

    core = BuildCore();
    if (!renderViews.Build(core, config.renderViews)) {
        return false;
    }
    commandRouter = std::make_shared<HostCommandRouter>(
        core, renderViews);
    renderViews.SetInitialVisibility();
    renderViews.SetInteractorsReady();
    endpoints = renderViews.BuildEndpoints();
    hotkeyRouter = std::make_unique<HostHotkeyRouter>(
        renderViews, commandRouter);
    ownerThread = std::this_thread::get_id();
    isBuilt = true;
    return true;
}

bool VtkAppHostSession::Impl::SendCommand(HostCommand command)
{
    return BuildSession()
        && commandRouter
        && commandRouter->DispatchCommand(std::move(command));
}

void VtkAppHostSession::Impl::DetachTimer()
{
    if (const auto context = timerContext.lock()) {
        context->ClearTimerHandler();
    }
    timerContext.reset();
}

bool VtkAppHostSession::Impl::AttachTimer(
    const HostTimerConfig& timerConfig)
{
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()) {
        return false;
    }
    if (!timerConfig.isTimerEnabled) {
        DetachTimer();
        return true;
    }
    const auto* timerView = renderViews.GetViewBySelector(
        timerConfig.targetView);
    if (!timerView || !timerView->context) {
        DetachTimer();
        return false;
    }
    DetachTimer();
    timerContext = timerView->context;
    timerView->context->SetTimerHandler([this]() {
        OnHostTimer();
    });
    return true;
}

void VtkAppHostSession::Impl::OnHostTimer()
{
    if (ownerThread != std::this_thread::get_id()) {
        return;
    }

    auto output = features.begin();
    for (auto input = features.begin();
        input != features.end(); ++input) {
        auto feature = input->feature.lock();
        if (!feature) {
            bool isInputDetached = false;
            try {
                isInputDetached = hotkeyRouter
                    && hotkeyRouter->GetInputPort().DetachInput(
                        input->id);
            }
            catch (...) {
                isInputDetached = false;
            }
            if (!isInputDetached) {
                std::cerr
                    << "[Host] Expired Feature input detach failed: "
                    << input->id << '\n';
                *output++ = *input;
            }
            continue;
        }
        *output++ = *input;
        try {
            (void)feature->OnHostTick();
        }
        catch (...) {
            std::cerr
                << "[Host] Feature tick failed: "
                << input->id << '\n';
        }
    }
    features.erase(output, features.end());

    std::vector<std::function<void()>> completes;
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        if (ownerCompleteState->isActive) {
            completes.swap(ownerCompleteState->completes);
        }
    }
    for (auto& complete : completes) {
        try {
            complete();
        }
        catch (...) {
        }
    }
}

bool VtkAppHostSession::Impl::AttachFeature(
    const std::shared_ptr<HostFeature>& feature)
{
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()
        || !feature
        || !hotkeyRouter) {
        return false;
    }

    std::string id;
    try {
        id = feature->GetFeatureId();
    }
    catch (...) {
        return false;
    }
    if (id.empty()) {
        return false;
    }
    for (const auto& entry : features) {
        const auto current = entry.feature.lock();
        if (entry.id == id
            || (current && current.get() == feature.get())) {
            return false;
        }
    }

    HostFeatureContext context;
    context.renderViews = &renderViews;
    context.inputPort = &hotkeyRouter->GetInputPort();
    context.getImageSnapshot = core.GetImageReader();
    context.setImageState = core.GetImageWriter();
    const std::weak_ptr<OwnerCompleteState> weakCompleteState =
        ownerCompleteState;
    context.sendOwnerComplete = [weakCompleteState](
        std::function<void()> complete) {
        const auto state = weakCompleteState.lock();
        if (!state || !complete) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->isActive) {
            return false;
        }
        state->completes.push_back(std::move(complete));
        return true;
    };

    try {
        if (!feature->AttachHost(context)) {
            return false;
        }
        features.push_back(
            FeatureEntry{ std::move(id), feature });
    }
    catch (...) {
        try {
            (void)feature->DetachHost();
        }
        catch (...) {
        }
        return false;
    }
    return true;
}

bool VtkAppHostSession::Impl::DetachFeature(
    const HostFeature& feature)
{
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()) {
        return false;
    }
    const auto entry = std::find_if(
        features.begin(),
        features.end(),
        [&feature](const FeatureEntry& current) {
            const auto value = current.feature.lock();
            return value && value.get() == &feature;
        });
    if (entry == features.end()) {
        return false;
    }
    try {
        if (!const_cast<HostFeature&>(feature).DetachHost()) {
            return false;
        }
    }
    catch (...) {
        return false;
    }
    features.erase(entry);
    return true;
}

bool VtkAppHostSession::Impl::DetachFeatures()
{
    for (auto entry = features.rbegin();
        entry != features.rend(); ++entry) {
        if (auto feature = entry->feature.lock()) {
            try {
                if (!feature->DetachHost()) {
                    return false;
                }
            }
            catch (...) {
                return false;
            }
        }
        else if (hotkeyRouter) {
            try {
                if (!hotkeyRouter->GetInputPort().DetachInput(
                        entry->id)) {
                    return false;
                }
            }
            catch (...) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    features.clear();
    return true;
}

VtkAppHostSession::VtkAppHostSession(HostSessionConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession() = default;
VtkAppHostSession::VtkAppHostSession(
    VtkAppHostSession&&) noexcept = default;
VtkAppHostSession& VtkAppHostSession::operator=(
    VtkAppHostSession&&) noexcept = default;

bool VtkAppHostSession::BuildSession()
{
    return m_impl && m_impl->BuildSession();
}

bool VtkAppHostSession::AttachTimer(
    const HostTimerConfig& config)
{
    return BuildSession() && m_impl->AttachTimer(config);
}

bool VtkAppHostSession::AttachHotkeys(
    const HostHotkeyConfig& config,
    HostHotkeyTemplates templates)
{
    return BuildSession()
        && m_impl->hotkeyRouter
        && m_impl->hotkeyRouter->AttachHotkeys(
            config, std::move(templates));
}

bool VtkAppHostSession::AttachFeature(
    const std::shared_ptr<HostFeature>& feature)
{
    return m_impl && m_impl->AttachFeature(feature);
}

bool VtkAppHostSession::DetachFeature(
    const HostFeature& feature)
{
    return m_impl && m_impl->DetachFeature(feature);
}

bool VtkAppHostSession::Start()
{
    if (!BuildSession() || m_impl->isStarted) {
        return false;
    }
    const auto* view =
        m_impl->renderViews.GetStandaloneStartView();
    if (!view || !view->context) {
        return false;
    }
    m_impl->renderViews.SendRenderAll();
    m_impl->isStarted = true;
    view->context->Start();
    return true;
}

bool VtkAppHostSession::SendData(
    HostDataRequest request,
    HostCompleteCallback onComplete)
{
    return m_impl->SendCommand(
        HostCommand{ HostDataCommand{
            std::move(request), std::move(onComplete) } });
}

bool VtkAppHostSession::SendView(HostViewRequest request)
{
    return m_impl->SendCommand(
        HostCommand{ HostViewCommand{
            std::move(request) } });
}

bool VtkAppHostSession::SendTool(HostToolRequest request)
{
    return m_impl->SendCommand(
        HostCommand{ HostToolCommand{
            std::move(request) } });
}

const std::vector<HostRenderViewEndpoint>&
VtkAppHostSession::GetRenderViewEndpoints()
{
    (void)BuildSession();
    return m_impl->endpoints;
}

const HostRenderViewEndpoint*
VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& viewId)
{
    (void)BuildSession();
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.id == viewId) {
            return &endpoint;
        }
    }
    return nullptr;
}

const HostRenderViewEndpoint*
VtkAppHostSession::GetPrimaryEndpoint()
{
    (void)BuildSession();
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role == HostRenderViewRole::Primary3D) {
            return &endpoint;
        }
    }
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role
            == HostRenderViewRole::Composite3D) {
            return &endpoint;
        }
    }
    return m_impl->endpoints.empty()
        ? nullptr : &m_impl->endpoints.front();
}
