#include "../AppRuntime.h"

class DataPortAdapter final : public AppDataPort {
public:
    explicit DataPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    TaskAdmissionResult LoadFileAsync(
        std::string path,
        VolumeLayout layout,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->LoadFileAsync(
                std::move(path), std::move(layout), std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ReloadFromBufferAsync(
                std::move(buffer), std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult StartDataActivation(const DataRevisionRef& ref, DataBindingRevision expected,
        std::function<void(bool)> onComplete) override {
        return m_service ? m_service->StartDataActivation(ref, expected, std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ExportDataAsync(
                std::move(outputDir),
                std::move(extension),
                std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

    TaskAdmissionResult ExportSlicesAsync(
        std::string path,
        std::optional<double> rotationAngleDeg,
        std::function<void(bool)> onComplete) override
    {
        return m_service
            ? m_service->ExportSlicesAsync(
                path, rotationAngleDeg, std::move(onComplete))
            : TaskAdmissionResult::Unavailable;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class DataStageAdapter final : public AppDataStagePort {
public:
    explicit DataStageAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool GetCanClearRenderInput() const override { return m_service && m_service->GetCanClearRenderInput(); }
    bool ClearRenderInput(const DataEntityId& entity) noexcept override {
        return m_service && m_service->ClearRenderInput(entity);
    }
    DataStageStatus StartRenderInputStage(const VtkRenderInputSnapshot& input,std::uint64_t revision) override {
        return m_service?m_service->StartDataStage(input,revision):DataStageStatus::Failed;
    }
    DataStageStatus StartRenderInputStage(const VtkRenderInputSnapshot& input,std::uint64_t revision,
        const std::optional<RenderEffectChange>& effect) override {
        return m_service?m_service->StartDataStage(input,revision,effect):DataStageStatus::Failed;
    }
    DataStageStatus SetRenderInputStageReady(const VtkRenderInputSnapshot& input,std::uint64_t revision) override {
        return m_service?m_service->SetDataStageReady(input,revision):DataStageStatus::Failed;
    }
    bool SetRenderInputViewStage(const VtkRenderInputSnapshot& input,std::uint64_t revision) override {
        return m_service&&m_service->SetViewStage(input,revision);
    }

    DataStageStatus StartDataStage(
        const VtkImageGridSnapshot& snapshot,
        const std::uint64_t transactionRevision) override
    {
        return m_service
            ? m_service->StartDataStage(VtkRenderInputView::FromImage(snapshot), transactionRevision)
            : DataStageStatus::Failed;
    }

    DataStageStatus SetDataStageReady(
        const VtkImageGridSnapshot& snapshot,
        const std::uint64_t transactionRevision) override
    {
        return m_service
            ? m_service->SetDataStageReady(VtkRenderInputView::FromImage(snapshot), transactionRevision)
            : DataStageStatus::Failed;
    }

    DataStageStatus GetDataStageStatus(
        const std::uint64_t transactionRevision) const override
    {
        return m_service
            ? m_service->GetDataStageStatus(transactionRevision)
            : DataStageStatus::Idle;
    }

    RenderEffectFailure GetDataStageFailure(std::uint64_t revision) const override {
        return m_service?m_service->GetDataStageFailure(revision):RenderEffectFailure::None;
    }

    bool SetViewStage(
        const VtkImageGridSnapshot& snapshot,
        const std::uint64_t transactionRevision) override
    {
        return m_service
            && m_service->SetViewStage(VtkRenderInputView::FromImage(snapshot), transactionRevision);
    }

    bool ResetViewStage(
        const std::uint64_t transactionRevision) override
    {
        return m_service
            && m_service->ResetViewStage(transactionRevision);
    }

    bool ClearDataStage(
        const std::uint64_t transactionRevision) override
    {
        return m_service
            && m_service->ClearDataStage(transactionRevision);
    }

    void SetDataStageComplete(
        const std::uint64_t transactionRevision) noexcept override
    {
        if (!m_service) std::terminate();
        m_service->SetDataStageComplete(transactionRevision);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class ViewPortAdapter final : public AppViewPort {
public:
    explicit ViewPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    ~ViewPortAdapter() override = default;

    bool SetViewConfig(const PreInitConfig& config) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable) return false;
        const VizMode oldMode = m_service->GetVizMode();
        if (!m_service->StartViewUpdate()) return false;
        try { m_service->SetVisualConfig(config); }
        catch (...) {
            m_service->SetVizMode(oldMode);
            UpdateFlags droppedFlags = UpdateFlags::None;
            const bool isClosed = m_service->SetViewUpdateCommit(
                false, droppedFlags);
            if (!isClosed) m_isAvailable = false;
            const auto service = m_service;
            lock.unlock();
            if (isClosed) {
                service->SendViewUpdateFlags(droppedFlags);
            }
            return false;
        }

        UpdateFlags pendingFlags = UpdateFlags::None;
        if (m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        m_service->SetVizMode(oldMode);
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    bool SendViewUpdate(const AppViewUpdate& update) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable || !GetUpdateValid(update)) {
            return false;
        }

        const AppViewState oldState = GetState();
        if (!m_service->StartViewUpdate()) return false;
        bool isUpdated = false;
        try { isUpdated = SetUpdate(update); }
        catch (...) { isUpdated = false; }
        UpdateFlags pendingFlags = UpdateFlags::None;
        if (isUpdated && m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        // 这是 owner-thread 补偿事务而非跨线程值原子提交：事件屏障保证
        // 失败更新及其回滚不会触发同步 observer 或可渲染中间帧。
        bool isRestored = false;
        try { isRestored = SetState(oldState); }
        catch (...) { isRestored = false; }
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isRestored || !isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    bool SetViewState(
        const AppViewState& state,
        const std::uint64_t expectedRevision) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_service || !m_isAvailable
            || m_revision != expectedRevision) {
            return false;
        }

        const AppViewState oldState = GetState();
        if (!m_service->StartViewUpdate()) return false;
        bool isUpdated = false;
        try { isUpdated = SetState(state); }
        catch (...) { isUpdated = false; }
        UpdateFlags pendingFlags = UpdateFlags::None;
        if (isUpdated && m_service->SetViewUpdateCommit(
                true, pendingFlags)) {
            ++m_revision;
            const auto service = m_service;
            lock.unlock();
            service->SendViewUpdateFlags(pendingFlags);
            return true;
        }

        bool isRestored = false;
        try { isRestored = SetState(oldState); }
        catch (...) { isRestored = false; }
        UpdateFlags droppedFlags = UpdateFlags::None;
        const bool isClosed = m_service->SetViewUpdateCommit(
            false, droppedFlags);
        if (!isRestored || !isClosed) m_isAvailable = false;
        const auto service = m_service;
        lock.unlock();
        if (isClosed) {
            service->SendViewUpdateFlags(droppedFlags);
        }
        return false;
    }

    AppViewState GetViewState() const override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return GetState();
    }

    RulerState GetRulerState() const override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_service ? m_service->m_ruler.GetState() : RulerState{};
    }

private:
    bool GetUpdateValid(const AppViewUpdate& update) const
    {
        if (update.ruler && !RulerMetrics::GetParamsValid(*update.ruler)) return false;
        const auto isUnit = [](const double value) {
            return std::isfinite(value) && value >= 0.0 && value <= 1.0;
        };
        if (update.mode) {
            switch (*update.mode) {
            case VizMode::Volume:
            case VizMode::IsoSurface:
            case VizMode::SliceTop_down:
            case VizMode::SliceFront_back:
            case VizMode::SliceLeft_right:
            case VizMode::CompositeVolume:
            case VizMode::CompositeIsoSurface:
                break;
            default:
                return false;
            }
        }
        if (update.material) {
            const auto& material = *update.material;
            if (!isUnit(material.ambient) || !isUnit(material.diffuse)
                || !isUnit(material.specular) || !isUnit(material.opacity)
                || !std::isfinite(material.specularPower)
                || material.specularPower < 0.0) {
                return false;
            }
        }
        if (update.opacity && !isUnit(*update.opacity)) return false;
        if (update.volumeTransferFunction
            && !m_service->GetVolumeTransferValid(
                *update.volumeTransferFunction)) {
            return false;
        }
        if (update.isoThreshold && !std::isfinite(*update.isoThreshold)) {
            return false;
        }
        if (update.background
            && (!isUnit(update.background->r)
                || !isUnit(update.background->g)
                || !isUnit(update.background->b))) {
            return false;
        }
        if (update.windowLevel
            && (!std::isfinite(update.windowLevel->windowWidth)
                || update.windowLevel->windowWidth <= 0.0
                || !std::isfinite(update.windowLevel->windowCenter))) {
            return false;
        }
        if (update.windowLevelMode) {
            switch (*update.windowLevelMode) {
            case WindowLevelMode::Auto:
            case WindowLevelMode::Manual:
                break;
            default:
                return false;
            }
        }
        if (update.volumeQuality) {
            switch (*update.volumeQuality) {
            case VolumeQuality::Auto:
            case VolumeQuality::Low:
            case VolumeQuality::High:
            case VolumeQuality::XHigh:
            case VolumeQuality::Ultra:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    bool SetUpdate(const AppViewUpdate& update)
    {
        // 先执行可能拒绝输入的写入；调用方持有完整快照并负责失败补偿。
        if (update.volumeQuality
            && !m_service->SetVolumeQuality(*update.volumeQuality)) {
            return false;
        }
        if (update.gradientOpacity
            && !m_service->SetGradientOpacity(*update.gradientOpacity)) {
            return false;
        }
        if (update.isDenoiseOn
            && !m_service->SetDenoiseOn(*update.isDenoiseOn)) {
            return false;
        }
        if (update.windowLevelMode) {
            if (*update.windowLevelMode == WindowLevelMode::Auto) {
                const bool isReset = update.windowLevel
                    ? m_service->ResetWindowLevel(*update.windowLevel)
                    : m_service->ResetWindowLevel();
                if (!isReset) return false;
            }
            else {
                const auto windowLevel = update.windowLevel
                    ? *update.windowLevel
                    : m_service->GetWindowLevel();
                m_service->SetWindowLevel(
                    windowLevel.windowWidth,
                    windowLevel.windowCenter);
            }
        }
        else if (update.windowLevel) {
            m_service->SetWindowLevel(
                update.windowLevel->windowWidth,
                update.windowLevel->windowCenter);
        }

        if (update.ruler) {
            m_service->m_rulerParams = *update.ruler;
            m_service->SendViewUpdateFlags(UpdateFlags::Visibility);
        }

        // 其余写入只在所有可失败字段通过后提交；optional 缺省保持当前状态。
        if (update.mode) m_service->SetVizMode(*update.mode);
        if (update.material) m_service->SetMaterial(*update.material);
        if (update.opacity) m_service->SetOpacity(*update.opacity);
        if (update.volumeTransferFunction
            && !m_service->SetVolumeTransferFunction(
                *update.volumeTransferFunction)) {
            return false;
        }
        if (update.isoThreshold) {
            m_service->SetIsoThreshold(*update.isoThreshold);
        }
        if (update.background) m_service->SetBackground(*update.background);
        if (update.visibility) {
            const auto& visibility = *update.visibility;
            if (visibility.isPlanes3DVisible) {
                m_service->SetElementVisible(
                    VisFlags::Planes3D,
                    *visibility.isPlanes3DVisible);
            }
            if (visibility.isCrosshairVisible) {
                m_service->SetElementVisible(
                    VisFlags::Crosshair,
                    *visibility.isCrosshairVisible);
            }
            if (visibility.isRulerVisible) {
                m_service->SetElementVisible(
                    VisFlags::Ruler,
                    *visibility.isRulerVisible);
            }
        }
        return true;
    }

    bool SetState(const AppViewState& state)
    {
        AppViewUpdate update;
        update.mode = state.mode;
        update.material = state.material;
        if (!state.isTransferAuto
            && (!state.volumeTransferFunction.colorNodes.empty()
            || !state.volumeTransferFunction.opacityNodes.empty())) {
            update.volumeTransferFunction =
                state.volumeTransferFunction;
        }
        update.isoThreshold = state.isoThreshold;
        update.background = state.background;
        update.windowLevel = state.windowLevel;
        update.windowLevelMode = state.windowLevelMode;
        update.volumeQuality = state.volumeQuality;
        update.gradientOpacity = state.gradientOpacity;
        update.isDenoiseOn = state.isDenoiseOn;
        update.ruler = state.ruler;
        AppVisibilityUpdate visibility;
        visibility.isPlanes3DVisible =
            (state.visibilityMask & VisFlags::Planes3D) != 0;
        visibility.isCrosshairVisible =
            (state.visibilityMask & VisFlags::Crosshair) != 0;
        visibility.isRulerVisible =
            (state.visibilityMask & VisFlags::Ruler) != 0;
        update.visibility = visibility;
        if (!GetUpdateValid(update) || !SetUpdate(update)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_service->m_transitionMutex);
            m_service->m_hasExplicitIso = state.hasExplicitIso;
        }
        return !state.isTransferAuto
            || m_service->ResetTransfer(
                state.volumeTransferFunction);
    }

    AppViewState GetState() const
    {
        AppViewState state;
        if (!m_service) return state;
        state.mode = m_service->GetVizMode();
        state.material = m_service->GetMaterial();
        state.volumeTransferFunction =
            m_service->GetVolumeTransferFunction();
        state.isTransferAuto = m_service->GetTransferAuto();
        state.isoThreshold = m_service->GetIsoThreshold();
        {
            std::lock_guard<std::mutex> lock(m_service->m_transitionMutex);
            state.hasExplicitIso = m_service->m_hasExplicitIso;
        }
        state.background = m_service->GetBackground();
        state.spacing = m_service->GetSpacing();
        state.windowLevel = m_service->GetWindowLevel();
        state.windowLevelMode = m_service->GetWindowLevelMode();
        state.scalarRange = m_service->GetScalarRange();
        state.volumeQuality = m_service->GetVolumeQuality();
        state.gradientOpacity = m_service->GetGradientOpacity();
        state.isFeatureActive = m_service->GetIsFeatureActive();
        state.isDenoiseOn = m_service->GetDenoiseOn();
        state.isInteracting = m_service->GetIsInteracting();
        state.cursorWorld = m_service->GetCursorWorld();
        state.cursorAxis = m_service->GetCursorAxis();
        state.visibilityMask = m_service->GetVisibilityMask();
        state.ruler = m_service->m_rulerParams;
        state.rulerState = m_service->m_ruler.GetState();
        state.dataRevision = m_service->GetRenderInputStamp().dataRevision;
        state.bindingRevision = m_service->m_renderSnapshot
            && m_service->m_renderSnapshot->binding
            ? m_service->m_renderSnapshot->binding->revision : 0;
        state.revision = m_revision;
        const bool hasTransitionFailed = m_service->GetHasTransitionFailure();
        state.hasPresentationFailed = !m_isAvailable || hasTransitionFailed;
        if (m_service->m_currentStrategy) {
            const auto transition = m_service->m_currentStrategy->GetTransitionState();
            state.hasPresentationFailed = state.hasPresentationFailed
                || transition.status == RenderProductStatus::Failed
                || transition.status == RenderProductStatus::Cancelled;
            state.isPresentationPending = !hasTransitionFailed && (m_service->m_hasDataRefreshNeed.load()
                || m_service->m_hasSyncNeed.load()
                || transition.status == RenderProductStatus::Preparing
                || transition.status == RenderProductStatus::Ready
                || (transition.status == RenderProductStatus::Active
                    && (transition.stats.isPreview
                        || transition.stats.activeRevision != transition.stats.requestRevision)));
        }
        return state;
    }

    std::shared_ptr<AppRuntime> m_service;
    mutable std::mutex m_mutex;
    std::uint64_t m_revision = 0;
    bool m_isAvailable = true;
};

class SessionPortAdapter final : public AppSessionPort {
public:
    explicit SessionPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetPrimaryData(
        const DataRevisionRef& dataRevision,
        const DataBindingRevision expectedBindingRevision) override
    {
        // owner thread 串行执行；提交会调用 observer，不能持有重入路径所需的锁。
        const auto service = m_service;
        return service && service->SetPrimaryData(
            dataRevision, expectedBindingRevision);
    }

    bool SendSessionUpdate(
        const AppSessionUpdate& update) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_service && m_service->SendSessionUpdate(update);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
    std::mutex m_mutex;
};

class FeaturePortAdapter final : public AppFeaturePort {
public:
    explicit FeaturePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetFeatureActive(
        const FeatureSource& source,
        bool isActive) override
    {
        return m_service
            && m_service->SetFeatureActive(source, isActive);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class UpdatePortAdapter final : public RenderUpdatePort {
public:
    explicit UpdatePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    std::optional<DataRevisionRef> GetNavigationSource() const override {
        return m_service?std::optional<DataRevisionRef>{m_service->GetRenderInputStamp().dataRevision}:std::nullopt;
    }
    bool SendUpdates() override
    {
        return m_service && m_service->SendUpdates();
    }

    bool SendPendingUpdates() override
    {
        return m_service && m_service->SendPendingUpdates();
    }

    void SendCompletions() override
    {
        if (m_service) m_service->SendCompletions();
    }

    bool SetRenderNeeded() override
    {
        if (!m_service) return false;
        m_service->SetDirty();
        return true;
    }

    bool ResetRenderNeeded() override
    {
        return m_service && m_service->ResetDirty();
    }

    bool GetRenderNeeded() const override
    {
        return m_service && m_service->GetDirty();
    }

    bool SetInteractionPhase() override
    {
        return m_service && m_service->SetInteractionPhase();
    }

    void SetRenderComplete(
        const std::uint64_t durationUs) noexcept override
    {
        if (m_service) m_service->SetRenderComplete(durationUs);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class StatePortAdapter final : public InteractionStatePort {
public:
    explicit StatePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) override
    {
        return m_service
            && m_service->SetInteracting(source, isInteracting);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class SlicePortAdapter final : public SliceInputPort {
public:
    explicit SlicePortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetSliceScroll(int delta) override
    {
        if (!m_service) return false;
        m_service->SetSliceScroll(delta);
        return true;
    }

    int GetPlaneAxis(vtkActor* actor) const override
    {
        return m_service ? m_service->GetPlaneAxis(actor) : -1;
    }

    bool SetCursorWorld(
        const std::array<double, 3>& worldPosition,
        int axis) override
    {
        if (!m_service) return false;
        auto nextPosition = worldPosition;
        m_service->SetCursorWorldPosition(nextPosition.data(), axis);
        return true;
    }

    std::array<double, 3> GetCursorWorld() const override
    {
        return m_service
            ? m_service->GetCursorWorld()
            : std::array<double, 3>{};
    }

    WindowLevelParams GetWindowLevel() const override
    {
        return m_service
            ? m_service->GetWindowLevel()
            : WindowLevelParams{};
    }

    bool SetWindowLevel(double width,double center) override { if(!m_service)return false;m_service->SetWindowLevel(width,center);return true; }
    bool SetWindowLevelDrag(
        int totalDx,
        int totalDy,
        int viewWidth,
        int viewHeight,
        double startWidth,
        double startCenter) override
    {
        if (!m_service) return false;
        m_service->SetWindowLevelDrag(
            totalDx,
            totalDy,
            viewWidth,
            viewHeight,
            startWidth,
            startCenter);
        return true;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class ModelPortAdapter final : public ModelInputPort, public FeatureModelTransformPort {
public:
    ModelPortAdapter(std::shared_ptr<AppRuntime> service,
        std::weak_ptr<SharedInteractionState> state)
        : m_service(std::move(service))
        , m_state(std::move(state))
    {
    }

    std::optional<ModelTransformSnapshot> GetTransformState() const override
    {
        const auto state = GetEditState();
        return state ? std::optional<ModelTransformSnapshot>(state->GetTransformState()) : std::nullopt;
    }
    std::optional<std::uint64_t> StartTransform(const ModelTransformSnapshot& expected) override
    {
        const auto state = GetEditState();
        return state ? state->StartTransform("Viewer3D",expected) : std::nullopt;
    }
    bool SetTransformPreview(std::uint64_t token,std::uint64_t sequence,
        const std::array<double,16>& matrix) override
    {
        const auto state = GetEditState();
        return state && state->SetTransformPreview("Viewer3D",token,sequence,matrix);
    }
    bool SetTransformCommit(std::uint64_t token,std::uint64_t sequence,
        const std::array<double,16>& matrix) override
    {
        const auto state = GetEditState();
        return state && state->SetTransformPreview("Viewer3D",token,sequence,matrix,true);
    }
    bool StopTransform(std::uint64_t token) override
    {
        const auto state = GetEditState();
        return state && state->StopTransform("Viewer3D",token);
    }

    vtkProp3D* GetMainProp() const override
    {
        return m_service ? m_service->GetMainProp() : nullptr;
    }
    bool GetPointVisible(const std::array<double,3>& world) const override {
        return m_service&&m_service->GetPointVisible(world);
    }

    std::array<double, 16> GetModelMatrix() const override
    {
        return m_service
            ? m_service->GetModelMatrix()
            : std::array<double, 16>{};
    }

    bool SetModelMatrix(
        const std::array<double, 16>& modelToWorld) override
    {
        if (!m_service) return false;
        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        matrix->DeepCopy(modelToWorld.data());
        return m_service->SetModelMatrix(matrix);
    }

private:
    std::shared_ptr<SharedInteractionState> GetEditState() const
    {
        return m_ownerThread == std::this_thread::get_id() ? m_state.lock() : nullptr;
    }
    std::shared_ptr<AppRuntime> m_service;
    std::weak_ptr<SharedInteractionState> m_state;
    std::thread::id m_ownerThread = std::this_thread::get_id();
};

class RenderBindAdapter final : public RenderBindPort {
public:
    explicit RenderBindAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetRenderTarget(
        vtkSmartPointer<vtkRenderWindow> window,
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        return m_service
            && m_service->SetRenderContext(
                std::move(window), std::move(renderer));
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class FeatureViewAdapter final : public FeatureViewService {
public:
    bool PollRenderResources() override {return m_service&&RenderFrameLifetime::PollAll();}
    explicit FeatureViewAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetInteracting(
        const InteractionSource& source,
        bool isInteracting) override
    {
        return m_service
            && m_service->SetInteracting(source, isInteracting);
    }

    std::optional<std::array<double, 16>>
        GetModelToWorld() const override
    {
        return m_service
            ? std::optional<std::array<double, 16>>(
                m_service->GetModelMatrix())
            : std::nullopt;
    }

    std::optional<std::array<double, 3>> GetWorldPosition(
        const std::array<double, 3>& modelPosition) const override
    {
        if (!m_service) return std::nullopt;
        std::array<double, 3> worldPosition{};
        m_service->GetWorldPositionFromModel(
            modelPosition.data(), worldPosition.data());
        return worldPosition;
    }

    std::optional<RenderInputStamp>
        GetRenderInputStamp() const override
    {
        return m_service
            ? std::optional<RenderInputStamp>(
                m_service->GetRenderInputStamp())
            : std::nullopt;
    }

    bool AttachRenderEffect(
        std::shared_ptr<RenderEffect> effect) override
    {
        return m_service
            && m_service->AttachRenderEffect(std::move(effect));
    }

    bool DetachRenderEffect(const RenderEffect* effect) override
    {
        return m_service && m_service->DetachRenderEffect(effect);
    }

    bool SetRenderNeeded() override
    {
        if (!m_service) return false;
        m_service->SetDirty();
        return true;
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class OverlayPortAdapter final : public OverlayService {
public:
    explicit OverlayPortAdapter(std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) override
    {
        return m_service
            && m_service->AttachOverlay(std::move(overlay));
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        if (m_service) {
            m_service->RemoveOverlay(std::move(overlay));
        }
    }

    void ClearOverlays() noexcept override
    {
        if (m_service) m_service->ClearOverlays();
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

class TaskControlAdapter final : public AppTaskControlPort {
public:
    explicit TaskControlAdapter(
        std::shared_ptr<AppRuntime> service)
        : m_service(std::move(service))
    {
    }

    bool SetDataTaskStopping() override
    {
        return m_service && m_service->SetDataTaskStopping();
    }

    bool SetTaskStopping() override
    {
        return m_service && m_service->SetTaskStopping();
    }

    bool StopTasks(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return m_service && m_service->StopTasks(deadline);
    }

private:
    std::shared_ptr<AppRuntime> m_service;
};

AppFactoryResult CreateAppPorts(AppServiceArgs args)
{
    const std::weak_ptr<SharedInteractionState> transformState = args.interactionState;
    if (!args.dataManager
        || !args.interactionState
        || !args.eventSource) {
        return {};
    }

    std::shared_ptr<AppRuntime> service;
    try {
        service = std::make_shared<AppRuntime>(std::move(args));
    }
    catch (const std::exception& error) {
        std::cerr << "[AppRuntime] Worker startup failed: "
            << error.what() << '\n';
        return {};
    }
    catch (...) {
        std::cerr << "[AppRuntime] Worker startup failed.\n";
        return {};
    }

    AppFactoryResult result;
    result.app.data =
        std::make_shared<DataPortAdapter>(service);
    result.app.view =
        std::make_shared<ViewPortAdapter>(service);
    result.app.session =
        std::make_shared<SessionPortAdapter>(service);
    result.app.feature =
        std::make_shared<FeaturePortAdapter>(service);
    result.interaction.update =
        std::make_shared<UpdatePortAdapter>(service);
    result.interaction.state =
        std::make_shared<StatePortAdapter>(service);
    result.interaction.slice =
        std::make_shared<SlicePortAdapter>(service);
    result.interaction.model =
        std::make_shared<ModelPortAdapter>(service, transformState);
    result.dataStage =
        std::make_shared<DataStageAdapter>(service);
    result.renderBind =
        std::make_shared<RenderBindAdapter>(service);
    result.featureView =
        std::make_shared<FeatureViewAdapter>(service);
    result.overlay =
        std::make_shared<OverlayPortAdapter>(service);
    result.taskControl =
        std::make_shared<TaskControlAdapter>(service);
    return result;
}
