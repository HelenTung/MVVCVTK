#include "App/AppState.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>

// SharedInteractionState 的线程安全存储体：setter 在锁内比较并提交值，随后在锁外仅发布 UpdateFlags。
// 因而观察者拿到的是“哪些维度变化”的通知，实际值仍需通过本类 getter 读取一致快照。
class SharedInteractionState::Impl {
public:
    struct ViewValues final {
        std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
        std::vector<TFNode> nodes{
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 } };
        TransferPreset transferPreset = TransferPreset::Manual;
        DataVersion transferPresetVersion = 0;
        double isoValue = 0.0;
        MaterialParams material;
        BackgroundColor background;
        WindowLevelParams windowLevel;
        WindowLevelMode windowLevelMode = WindowLevelMode::Auto;
        uint32_t visibilityMask = VisFlags::Planes3D
            | VisFlags::Crosshair | VisFlags::Ruler;
        std::array<double, 3> cursorWorld = { 0.0, 0.0, 0.0 };
        std::array<double, 3> cursorRawWorld = { 0.0, 0.0, 0.0 };
        int cursorAxis = -1;
    };

    explicit Impl(std::shared_ptr<IStateEventSink> eventSink)
        : m_eventSink(std::move(eventSink))
    {
    }

    bool GetIsViewOwner() const noexcept
    {
        return m_isViewUpdateStarted
            && m_viewUpdateThread == std::this_thread::get_id();
    }

    ViewValues& GetViewValues() noexcept
    {
        return GetIsViewOwner() ? m_viewShadow : m_viewValues;
    }

    const ViewValues& GetViewValues() const noexcept
    {
        return GetIsViewOwner() ? m_viewShadow : m_viewValues;
    }

    void SetViewChanged(
        const UpdateFlags flag,
        const bool hasChanged) noexcept
    {
        if (!hasChanged || GetIsViewOwner()) return;
        SetRealViewChanged(flag, true);
    }

    void SetRealViewChanged(
        const UpdateFlags flag,
        const bool hasChanged) noexcept
    {
        if (!hasChanged) return;
        ++m_viewRevision;
        if (m_isViewUpdateStarted) {
            m_externalValueFlags |= flag;
        }
    }

    static bool SetScalar(double& current, double next, double epsilon = 1e-6)
    {
        if (std::abs(current - next) <= epsilon) return false;
        current = next;
        return true;
    }

    template <typename T>
    static bool SetValue(T& current, const T& next)
    {
        if (current == next) return false;
        current = next;
        return true;
    }

    template <std::size_t Count>
    static bool SetArray(
        std::array<double, Count>& current,
        const std::array<double, Count>& next,
        double epsilon = 1e-6)
    {
        for (std::size_t index = 0; index < Count; ++index) {
            if (std::abs(current[index] - next[index]) > epsilon) {
                current = next;
                return true;
            }
        }
        return false;
    }

    static bool SetTFNodes(
        std::vector<TFNode>& current,
        const std::vector<TFNode>& next)
    {
        if (current.size() == next.size()) {
            const bool isSame = std::equal(
                current.begin(), current.end(), next.begin(),
                [](const TFNode& left, const TFNode& right) {
                    return std::abs(left.position - right.position) <= 1e-6
                        && std::abs(left.opacity - right.opacity) <= 1e-6
                        && std::abs(left.r - right.r) <= 1e-6
                        && std::abs(left.g - right.g) <= 1e-6
                        && std::abs(left.b - right.b) <= 1e-6;
                });
            if (isSame) return false;
        }
        current = next;
        return true;
    }

    static bool SetMaterial(MaterialParams& current, const MaterialParams& next)
    {
        if (std::abs(current.ambient - next.ambient) <= 1e-6
            && std::abs(current.diffuse - next.diffuse) <= 1e-6
            && std::abs(current.specular - next.specular) <= 1e-6
            && std::abs(current.specularPower - next.specularPower) <= 1e-6
            && std::abs(current.opacity - next.opacity) <= 1e-6
            && current.isShadeOn == next.isShadeOn) {
            return false;
        }
        current = next;
        return true;
    }

    static bool SetBackground(BackgroundColor& current, const BackgroundColor& next)
    {
        if (std::abs(current.r - next.r) <= 1e-6
            && std::abs(current.g - next.g) <= 1e-6
            && std::abs(current.b - next.b) <= 1e-6) {
            return false;
        }
        current = next;
        return true;
    }

    static bool SetWindowLevel(
        WindowLevelParams& current,
        const WindowLevelParams& next)
    {
        if (std::abs(current.windowWidth - next.windowWidth) <= 1e-6
            && std::abs(current.windowCenter - next.windowCenter) <= 1e-6) {
            return false;
        }
        current = next;
        return true;
    }

    static bool SetVisibilityMask(
        uint32_t& current,
        uint32_t flagBit,
        bool isVisible)
    {
        const uint32_t next = isVisible ? current | flagBit : current & ~flagBit;
        if (next == current) return false;
        current = next;
        return true;
    }

    void SendDirect(UpdateFlags flags) noexcept
    {
        std::shared_ptr<IStateEventSink> eventSink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            eventSink = m_eventSink;
        }
        if (!eventSink || flags == UpdateFlags::None) return;
        try { eventSink->SendFlags(flags); }
        catch (...) {
            // 状态提交与 observer 通知是两个结果；外部异常不得反向改变提交语义。
        }
    }

    void SendFlags(UpdateFlags flags) noexcept
    {
        std::shared_ptr<IStateEventSink> eventSink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_isViewUpdateStarted) {
                if (m_viewUpdateThread == std::this_thread::get_id()) {
                    m_ownerUpdateFlags |= flags;
                }
                else {
                    m_externalUpdateFlags |= flags;
                }
                return;
            }
            eventSink = m_eventSink;
        }
        // 外部观察者可能回读状态，因此必须在状态锁外广播。
        if (!eventSink || flags == UpdateFlags::None) return;
        try { eventSink->SendFlags(flags); }
        catch (...) {
            // 非事务 setter 同样隔离 observer；已提交的值不能被伪装成失败。
        }
    }

    mutable std::mutex m_mutex;
    std::mutex m_spacingMutex; // 串行 DataManager spacing 副作用与共享状态提交。
    std::shared_ptr<IStateEventSink> m_eventSink;
    LoadState m_dataTrustedState = LoadState::Idle; // 当前可供渲染的数据是否可信；Reload 期间可继续为 Succeeded。
    LoadState m_fileLoadState = LoadState::Idle;    // 文件加载通道的最近状态。
    LoadState m_reloadLoadState = LoadState::Idle;  // 内存重载通道的最近状态。
    LoadEventKind m_activeLoadKind = LoadEventKind::None; // 全局 admission：File/Reload 同时最多一个在途事务。
    bool m_isLoadPublished = false;  // 终态广播已完成，owner 可以 ResetLoad 释放 admission。
    bool m_isLoadPublishing = false; // 广播窗口保护；防止回调重入提前重置当前事务。
    bool m_isViewUpdateStarted = false; // owner-thread View 补偿期间阻止中间 flags 外泄。
    std::thread::id m_viewUpdateThread; // 仅屏障创建线程可关闭本次事务。
    UpdateFlags m_ownerUpdateFlags = UpdateFlags::None; // owner flags 随事务成败提交或丢弃。
    UpdateFlags m_externalUpdateFlags = UpdateFlags::None; // 并发 writer flags 在事务结束后发布。
    UpdateFlags m_externalValueFlags = UpdateFlags::None; // 并发 writer 已改 committed 值；用于 CAS 与副作用补偿。
    std::uint64_t m_viewRevision = 0; // committed ViewValues 的单调版本。
    std::uint64_t m_viewBaseRevision = 0; // owner shadow 创建时对应的 committed 版本。
    ViewValues m_viewValues; // worker 与非事务调用只写 committed。
    ViewValues m_viewShadow; // owner View 事务只写 shadow，失败时直接丢弃。
    DataVersion m_dataVersion = 0; // 与已完成全局共享提交的数据批次一致。
    std::array<double, 2> m_dataRange = { 0.0, 255.0 }; // 当前标量 min/max，供 TF、ISO 与默认窗宽窗位使用。
    std::vector<InteractionSource> m_activeSources; // 非空时使用交互刷新率；来源独立退出互不覆盖。
    std::array<double, 16> m_modelMatrix = { // 行主序 4x4 modelToWorld affine。
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
};

// 展示状态复用同一套值比较与 shadow/revision 事务实现，但持有独立存储和独立事件出口。
// 该组合只暴露 ViewPresentationState 的展示 API，Session 调用方无法取得其数据联动能力。
class ViewPresentationState::Impl {
public:
    explicit Impl(std::shared_ptr<IStateEventSink> eventSink)
        : storage(std::move(eventSink))
    {
    }

    SharedInteractionState storage;
};

SharedInteractionState::SharedInteractionState(
    std::shared_ptr<IStateEventSink> eventSink)
    : m_impl(std::make_unique<Impl>(std::move(eventSink)))
{
}

SharedInteractionState::~SharedInteractionState() = default;

void SharedInteractionState::SetEventSink(std::shared_ptr<IStateEventSink> eventSink)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_eventSink = std::move(eventSink);
}

bool SharedInteractionState::StartViewUpdate()
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if (m_impl->m_isViewUpdateStarted) return false;
    try {
        m_impl->m_viewShadow = m_impl->m_viewValues;
    }
    catch (...) {
        return false;
    }
    m_impl->m_isViewUpdateStarted = true;
    m_impl->m_viewUpdateThread = std::this_thread::get_id();
    m_impl->m_ownerUpdateFlags = UpdateFlags::None;
    m_impl->m_externalUpdateFlags = UpdateFlags::None;
    m_impl->m_externalValueFlags = UpdateFlags::None;
    m_impl->m_viewBaseRevision = m_impl->m_viewRevision;
    return true;
}

bool SharedInteractionState::SetViewUpdateCommit(const bool isCommitted)
{
    UpdateFlags flags = UpdateFlags::None;
    if (!SetViewUpdateCommit(isCommitted, flags)) return false;
    SendViewUpdateFlags(flags);
    return true;
}

bool SharedInteractionState::SetViewUpdateCommit(
    const bool isCommitted,
    UpdateFlags& pendingFlags)
{
    pendingFlags = UpdateFlags::None;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (!m_impl->m_isViewUpdateStarted
            || m_impl->m_viewUpdateThread
                != std::this_thread::get_id()) {
            return false;
        }
        if (isCommitted
            && m_impl->m_viewRevision
                != m_impl->m_viewBaseRevision) {
            // committed 在事务期间已被 worker 改写；保持事务打开，
            // 由 adapter 补偿本地副作用后再以 rollback 关闭。
            return false;
        }
        pendingFlags = m_impl->m_externalUpdateFlags
            | m_impl->m_externalValueFlags;
        if (isCommitted) {
            m_impl->m_viewValues = std::move(m_impl->m_viewShadow);
            ++m_impl->m_viewRevision;
            pendingFlags |= m_impl->m_ownerUpdateFlags;
        }
        m_impl->m_isViewUpdateStarted = false;
        m_impl->m_viewUpdateThread = {};
        m_impl->m_ownerUpdateFlags = UpdateFlags::None;
        m_impl->m_externalUpdateFlags = UpdateFlags::None;
        m_impl->m_externalValueFlags = UpdateFlags::None;
        m_impl->m_viewBaseRevision = m_impl->m_viewRevision;
    }
    return true;
}

void SharedInteractionState::SendViewUpdateFlags(
    const UpdateFlags flags) noexcept
{
    m_impl->SendDirect(flags);
}

LoadState SharedInteractionState::GetFileLoadState() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_fileLoadState;
}

LoadState SharedInteractionState::GetReloadLoadState() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_reloadLoadState;
}

LoadState SharedInteractionState::GetDataTrustedState() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_dataTrustedState;
}

bool SharedInteractionState::StartLoad(LoadEventKind loadEventKind)
{
    // A. 非 File/Reload 或已有事务时拒绝，确保两个加载通道共享一个串行 admission。
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if ((loadEventKind != LoadEventKind::File
        && loadEventKind != LoadEventKind::Reload)
        || m_impl->m_activeLoadKind != LoadEventKind::None) {
        return false;
    }

    m_impl->m_activeLoadKind = loadEventKind;
    m_impl->m_isLoadPublished = false;
    m_impl->m_isLoadPublishing = false;
    if (loadEventKind == LoadEventKind::File) {
        // B. File 会替换数据真源，加载期间 current 不再被标记为可信。
        m_impl->m_fileLoadState = LoadState::Loading;
        m_impl->m_dataTrustedState = LoadState::Loading;
    }
    else {
        // C. Reload 采用 pending 提交；加载期间仍允许旧 current 保持可信并继续显示。
        m_impl->m_reloadLoadState = LoadState::Loading;
    }
    return true;
}

bool SharedInteractionState::ResetLoad(LoadEventKind loadEventKind)
{
    // 只有匹配事务且终态已完整广播后才能释放 admission；广播回调重入会被 publishing 拦截。
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if (loadEventKind == LoadEventKind::None
        || m_impl->m_activeLoadKind != loadEventKind) {
        return false;
    }
    if (m_impl->m_isLoadPublishing || !m_impl->m_isLoadPublished) {
        return false;
    }
    m_impl->m_activeLoadKind = LoadEventKind::None;
    m_impl->m_isLoadPublished = false;
    m_impl->m_isLoadPublishing = false;
    return true;
}

bool SharedInteractionState::SetFileDataReady(
    double rangeMin,
    double rangeMax,
    const std::array<double, 3>& spacing)
{
    // 终态发布分三段：1. 锁内提交数据派生状态并标记 publishing；
    // 2. 锁外广播，允许观察者安全回读；3. 再次加锁标记 published，等待 owner ResetLoad。
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind != LoadEventKind::File
            || m_impl->m_isLoadPublishing
            || m_impl->m_fileLoadState != LoadState::Loading) return false;
        m_impl->m_isLoadPublishing = true;
        m_impl->m_dataRange = { rangeMin, rangeMax };
        auto& view = m_impl->m_viewValues;
        const bool hasSpacingChanged = Impl::SetArray(
            view.spacing, spacing);
        m_impl->m_fileLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->SetRealViewChanged(
            UpdateFlags::Spacing, hasSpacingChanged);
    }
    try { m_impl->SendFlags(UpdateFlags::DataReady | UpdateFlags::FileLoad); }
    catch (...) {}
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::File) {
            m_impl->m_isLoadPublished = true;
        }
        m_impl->m_isLoadPublishing = false;
    }
    return true;
}

bool SharedInteractionState::SetReloadDataReady(
    double rangeMin,
    double rangeMax,
    const std::array<double, 3>& spacing)
{
    // 与 File 成功路径使用同一发布协议；差异是 Reload 直到成功提交才替换 dataTrustedState。
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind != LoadEventKind::Reload
            || m_impl->m_isLoadPublishing
            || m_impl->m_reloadLoadState != LoadState::Loading) return false;
        m_impl->m_isLoadPublishing = true;
        m_impl->m_dataRange = { rangeMin, rangeMax };
        auto& view = m_impl->m_viewValues;
        const bool hasSpacingChanged = Impl::SetArray(
            view.spacing, spacing);
        m_impl->m_reloadLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->SetRealViewChanged(
            UpdateFlags::Spacing, hasSpacingChanged);
    }
    try { m_impl->SendFlags(UpdateFlags::DataReady | UpdateFlags::ReloadLoad); }
    catch (...) {}
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::Reload) {
            m_impl->m_isLoadPublished = true;
        }
        m_impl->m_isLoadPublishing = false;
    }
    return true;
}

bool SharedInteractionState::SetImageDataReady(
    double rangeMin,
    double rangeMax,
    const std::array<double, 3>& spacing)
{
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_dataRange = { rangeMin, rangeMax };
        const bool hasSpacingChanged = Impl::SetArray(
            m_impl->m_viewValues.spacing, spacing);
        m_impl->SetRealViewChanged(
            UpdateFlags::Spacing, hasSpacingChanged);
        m_impl->m_dataTrustedState = LoadState::Succeeded;
    }
    try {
        m_impl->SendFlags(UpdateFlags::DataReady);
    }
    catch (...) {
        // current 快照已发布；observer 异常不能把成功的 CAS 伪装成失败，
        // 否则调用方会回滚自己的状态机而 DataManager 已无法回滚。
    }
    return true;
}

void SharedInteractionState::SetDataReady(
    const DataReadyState& state) noexcept
{
    LoadEventKind loadKind = LoadEventKind::None;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        loadKind = m_impl->m_activeLoadKind;
        m_impl->m_isLoadPublishing =
            loadKind == LoadEventKind::File
            || loadKind == LoadEventKind::Reload;
        m_impl->m_dataVersion = state.version;
        m_impl->m_dataRange = state.scalarRange;
        auto& view = m_impl->m_viewValues;
        const bool hasSpacingChanged = Impl::SetArray(
            view.spacing, state.spacing);
        const bool hasRawCursorChanged = Impl::SetArray(
            view.cursorRawWorld, state.cursorWorld, 1e-9);
        const bool hasCursorChanged = Impl::SetArray(
            view.cursorWorld, state.cursorWorld, 1e-9);
        const bool hasAxisChanged = view.cursorAxis != -1;
        view.cursorAxis = -1;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->SetRealViewChanged(
            UpdateFlags::Spacing, hasSpacingChanged);
        m_impl->SetRealViewChanged(
            UpdateFlags::Cursor,
            hasRawCursorChanged || hasCursorChanged || hasAxisChanged);
        if (loadKind == LoadEventKind::File) {
            m_impl->m_fileLoadState = LoadState::Succeeded;
        }
        else if (loadKind == LoadEventKind::Reload) {
            m_impl->m_reloadLoadState = LoadState::Succeeded;
        }
    }

    UpdateFlags flags = UpdateFlags::DataReady
        | UpdateFlags::Cursor | UpdateFlags::Spacing;
    if (loadKind == LoadEventKind::File) {
        flags |= UpdateFlags::FileLoad;
    }
    else if (loadKind == LoadEventKind::Reload) {
        flags |= UpdateFlags::ReloadLoad;
    }
    m_impl->SendFlags(flags);

    if (loadKind != LoadEventKind::None) {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == loadKind) {
            m_impl->m_isLoadPublished = true;
        }
        m_impl->m_isLoadPublishing = false;
    }
}

DataVersion SharedInteractionState::GetDataVersion() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_dataVersion;
}

bool SharedInteractionState::SetFileLoadFailed()
{
    // File 失败意味着没有可信的新真源，因此 file 与 dataTrusted 同时进入 Failed。
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind != LoadEventKind::File
            || m_impl->m_isLoadPublishing
            || m_impl->m_fileLoadState != LoadState::Loading) return false;
        m_impl->m_isLoadPublishing = true;
        m_impl->m_fileLoadState = LoadState::Failed;
        m_impl->m_dataTrustedState = LoadState::Failed;
    }
    try { m_impl->SendFlags(UpdateFlags::LoadFailed | UpdateFlags::FileLoad); }
    catch (...) {}
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::File) {
            m_impl->m_isLoadPublished = true;
        }
        m_impl->m_isLoadPublishing = false;
    }
    return true;
}

bool SharedInteractionState::SetReloadLoadFailed()
{
    // Reload 失败只结束 reload 通道；若旧 current 仍可信，必须保留 dataTrustedState=Succeeded。
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind != LoadEventKind::Reload
            || m_impl->m_isLoadPublishing
            || m_impl->m_reloadLoadState != LoadState::Loading) return false;
        m_impl->m_isLoadPublishing = true;
        m_impl->m_reloadLoadState = LoadState::Failed;
        if (m_impl->m_dataTrustedState != LoadState::Succeeded) {
            m_impl->m_dataTrustedState = LoadState::Failed;
        }
    }
    try { m_impl->SendFlags(UpdateFlags::LoadFailed | UpdateFlags::ReloadLoad); }
    catch (...) {}
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::Reload) {
            m_impl->m_isLoadPublished = true;
        }
        m_impl->m_isLoadPublishing = false;
    }
    return true;
}

void SharedInteractionState::SetPreInitConfig(const PreInitConfig& config)
{
    UpdateFlags flags = UpdateFlags::None;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        if (Impl::SetMaterial(view.material, config.material)) {
            flags |= UpdateFlags::Material;
            m_impl->SetViewChanged(UpdateFlags::Material, true);
        }
        if (config.hasTF) {
            const bool hasIntentChanged =
                view.transferPreset != TransferPreset::Manual
                || view.transferPresetVersion != 0;
            view.transferPreset = TransferPreset::Manual;
            view.transferPresetVersion = 0;
            const bool hasNodesChanged = Impl::SetTFNodes(
                view.nodes, config.tfNodes);
            if (hasNodesChanged) {
                flags |= UpdateFlags::TF;
            }
            m_impl->SetViewChanged(
                UpdateFlags::TF,
                hasNodesChanged || hasIntentChanged);
        }
        if (config.hasIso
            && Impl::SetScalar(view.isoValue, config.isoThreshold)) {
            flags |= UpdateFlags::IsoValue;
            m_impl->SetViewChanged(UpdateFlags::IsoValue, true);
        }
        if (config.hasBgColor
            && Impl::SetBackground(view.background, config.bgColor)) {
            flags |= UpdateFlags::Background;
            m_impl->SetViewChanged(UpdateFlags::Background, true);
        }
        if (config.hasSpacing
            && Impl::SetArray(view.spacing, config.spacing)) {
            flags |= UpdateFlags::Spacing;
            m_impl->SetViewChanged(UpdateFlags::Spacing, true);
        }
        if (config.hasWindowLevel) {
            const bool hasModeChanged =
                view.windowLevelMode != WindowLevelMode::Manual;
            view.windowLevelMode = WindowLevelMode::Manual;
            const bool hasValueChanged = Impl::SetWindowLevel(
                view.windowLevel, config.windowLevel);
            if (hasModeChanged || hasValueChanged) {
                flags |= UpdateFlags::WindowLevel;
                m_impl->SetViewChanged(
                    UpdateFlags::WindowLevel, true);
            }
        }
    }
    if (flags != UpdateFlags::None) m_impl->SendFlags(flags);
}

void SharedInteractionState::SetModelMatrix(
    const std::array<double, 16>& modelToWorldMatrix)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(m_impl->m_modelMatrix, modelToWorldMatrix, 1e-9);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Transform);
}

std::array<double, 16> SharedInteractionState::GetModelMatrix() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_modelMatrix;
}

void SharedInteractionState::SetScalarRange(double rangeMin, double rangeMax)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(m_impl->m_dataRange, { rangeMin, rangeMax });
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::TF);
}

std::array<double, 2> SharedInteractionState::GetScalarRange() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_dataRange;
}

std::array<double, 2> SharedInteractionState::GetDataRange() const
{
    return GetScalarRange();
}

void SharedInteractionState::SetTFNodes(const std::vector<TFNode>& nodes)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        const bool hasIntentChanged =
            view.transferPreset != TransferPreset::Manual
            || view.transferPresetVersion != 0;
        view.transferPreset = TransferPreset::Manual;
        view.transferPresetVersion = 0;
        hasChanged = Impl::SetTFNodes(view.nodes, nodes);
        m_impl->SetViewChanged(
            UpdateFlags::TF,
            hasChanged || hasIntentChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::TF);
}

void SharedInteractionState::GetTFNodes(std::vector<TFNode>& destination) const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    destination = m_impl->GetViewValues().nodes;
}

void SharedInteractionState::SetTransferPresetIntent(TransferPreset preset)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    auto& view = m_impl->GetViewValues();
    if (view.transferPreset == preset
        && view.transferPresetVersion == 0) return;
    view.transferPreset = preset;
    view.transferPresetVersion = 0;
    m_impl->SetViewChanged(UpdateFlags::TF, true);
}

bool SharedInteractionState::SetTransferPresetNodes(
    TransferPreset preset,
    DataVersion dataVersion,
    const std::vector<TFNode>& nodes)
{
    if (preset == TransferPreset::Manual
        || dataVersion == 0
        || nodes.empty()) {
        return false;
    }

    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        if (view.transferPreset != preset
            || view.transferPresetVersion > dataVersion) {
            return false;
        }
        const bool hasVersionChanged =
            view.transferPresetVersion != dataVersion;
        hasChanged = Impl::SetTFNodes(view.nodes, nodes);
        view.transferPresetVersion = dataVersion;
        m_impl->SetViewChanged(
            UpdateFlags::TF,
            hasChanged || hasVersionChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::TF);
    return true;
}

TransferPreset SharedInteractionState::GetTransferPreset() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().transferPreset;
}

void SharedInteractionState::SetIsoValue(double value)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetScalar(
            m_impl->GetViewValues().isoValue, value);
        m_impl->SetViewChanged(
            UpdateFlags::IsoValue, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::IsoValue);
}

double SharedInteractionState::GetIsoValue() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().isoValue;
}

void SharedInteractionState::SetMaterial(const MaterialParams& material)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetMaterial(
            m_impl->GetViewValues().material, material);
        m_impl->SetViewChanged(
            UpdateFlags::Material, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Material);
}

MaterialParams SharedInteractionState::GetMaterial() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().material;
}

void SharedInteractionState::SetBackground(const BackgroundColor& background)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetBackground(
            m_impl->GetViewValues().background, background);
        m_impl->SetViewChanged(
            UpdateFlags::Background, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Background);
}

BackgroundColor SharedInteractionState::GetBackground() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().background;
}

void SharedInteractionState::SetSpacing(
    double spacingX,
    double spacingY,
    double spacingZ)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(
            m_impl->GetViewValues().spacing,
            { spacingX, spacingY, spacingZ });
        m_impl->SetViewChanged(
            UpdateFlags::Spacing, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Spacing);
}

bool SharedInteractionState::SetSpacingData(
    const std::array<double, 3>& spacing,
    const std::function<bool(
        const std::array<double, 3>&)>& setData)
{
    const std::lock_guard<std::mutex> spacingLock(
        m_impl->m_spacingMutex);
    bool hasChanged = false;
    bool hasExternalSpacing = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        hasChanged = !std::equal(
            view.spacing.begin(), view.spacing.end(),
            spacing.begin(),
            [](const double current, const double next) {
                return std::abs(current - next) <= 1e-6;
            });
        if (!hasChanged) return true;

        hasExternalSpacing = m_impl->GetIsViewOwner()
            && (m_impl->m_externalValueFlags & UpdateFlags::Spacing)
                != UpdateFlags::None;
    }
    if (!hasExternalSpacing
        && setData && !setData(spacing)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        hasChanged = Impl::SetArray(view.spacing, spacing);
        if (!hasChanged) return true;
        m_impl->SetViewChanged(UpdateFlags::Spacing, true);
    }
    m_impl->SendFlags(UpdateFlags::Spacing);
    return true;
}

std::array<double, 3> SharedInteractionState::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().spacing;
}

void SharedInteractionState::SetWindowLevel(double windowWidth, double windowCenter)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        const bool hasModeChanged =
            view.windowLevelMode != WindowLevelMode::Manual;
        view.windowLevelMode = WindowLevelMode::Manual;
        const bool hasValueChanged = Impl::SetWindowLevel(
            view.windowLevel,
            { windowWidth, windowCenter });
        hasChanged = hasModeChanged || hasValueChanged;
        m_impl->SetViewChanged(
            UpdateFlags::WindowLevel, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::WindowLevel);
}

bool SharedInteractionState::SetAutoWindowLevel(
    const WindowLevelParams& windowLevel)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        if (view.windowLevelMode != WindowLevelMode::Auto) {
            return false;
        }
        hasChanged = Impl::SetWindowLevel(
            view.windowLevel, windowLevel);
        m_impl->SetViewChanged(
            UpdateFlags::WindowLevel, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::WindowLevel);
    return true;
}

bool SharedInteractionState::ResetWindowLevel(
    const WindowLevelParams& windowLevel)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        auto& view = m_impl->GetViewValues();
        const bool hasModeChanged =
            view.windowLevelMode != WindowLevelMode::Auto;
        view.windowLevelMode = WindowLevelMode::Auto;
        const bool hasValueChanged = Impl::SetWindowLevel(
            view.windowLevel, windowLevel);
        hasChanged = hasModeChanged || hasValueChanged;
        m_impl->SetViewChanged(
            UpdateFlags::WindowLevel, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::WindowLevel);
    return true;
}

WindowLevelParams SharedInteractionState::GetWindowLevel() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().windowLevel;
}

WindowLevelMode SharedInteractionState::GetWindowLevelMode() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().windowLevelMode;
}

bool SharedInteractionState::SetInteracting(
    const InteractionSource& source,
    bool isInteracting)
{
    if (source.ownerId.empty() || source.channelId.empty()) {
        return false;
    }

    bool hasBoundaryChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        const bool wasInteracting = !m_impl->m_activeSources.empty();
        const auto sourceIt = std::find(
            m_impl->m_activeSources.begin(),
            m_impl->m_activeSources.end(),
            source);
        if (isInteracting && sourceIt == m_impl->m_activeSources.end()) {
            m_impl->m_activeSources.push_back(source);
        }
        else if (!isInteracting && sourceIt != m_impl->m_activeSources.end()) {
            m_impl->m_activeSources.erase(sourceIt);
        }
        hasBoundaryChanged =
            wasInteracting != !m_impl->m_activeSources.empty();
    }
    if (hasBoundaryChanged) {
        m_impl->SendFlags(UpdateFlags::RenderRate);
    }
    return true;
}

bool SharedInteractionState::GetIsInteracting() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return !m_impl->m_activeSources.empty();
}

void SharedInteractionState::SetCursorWorld(double worldX, double worldY, double worldZ)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(
            m_impl->GetViewValues().cursorWorld,
            { worldX, worldY, worldZ }, 1e-9);
        m_impl->SetViewChanged(
            UpdateFlags::Cursor, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Cursor);
}

void SharedInteractionState::SetCursorRawWorld(
    double worldX,
    double worldY,
    double worldZ)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    auto& view = m_impl->GetViewValues();
    const bool hasChanged = Impl::SetArray(
        view.cursorRawWorld,
        { worldX, worldY, worldZ }, 1e-9);
    m_impl->SetViewChanged(UpdateFlags::Cursor, hasChanged);
}

std::array<double, 3> SharedInteractionState::GetCursorRawWorld() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().cursorRawWorld;
}

void SharedInteractionState::SetCursorAxis(int axis)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    auto& view = m_impl->GetViewValues();
    const bool hasChanged = view.cursorAxis != axis;
    view.cursorAxis = axis;
    m_impl->SetViewChanged(UpdateFlags::Cursor, hasChanged);
}

int SharedInteractionState::GetCursorAxis() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().cursorAxis;
}

std::array<double, 3> SharedInteractionState::GetCursorWorld() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().cursorWorld;
}

void SharedInteractionState::SetElementVisible(uint32_t flagBit, bool isVisible)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetVisibilityMask(
            m_impl->GetViewValues().visibilityMask,
            flagBit, isVisible);
        m_impl->SetViewChanged(
            UpdateFlags::Visibility, hasChanged);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Visibility);
}

uint32_t SharedInteractionState::GetVisibilityMask() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->GetViewValues().visibilityMask;
}

ViewPresentationState::ViewPresentationState(
    std::shared_ptr<IStateEventSink> eventSink)
    : m_impl(std::make_unique<Impl>(std::move(eventSink)))
{
}

ViewPresentationState::~ViewPresentationState() = default;

bool ViewPresentationState::StartUpdate()
{
    return m_impl->storage.StartViewUpdate();
}

bool ViewPresentationState::SetUpdateCommit(
    const bool isCommitted,
    UpdateFlags& pendingFlags)
{
    return m_impl->storage.SetViewUpdateCommit(
        isCommitted, pendingFlags);
}

void ViewPresentationState::SendUpdateFlags(
    const UpdateFlags flags) noexcept
{
    m_impl->storage.SendViewUpdateFlags(flags);
}

void ViewPresentationState::SetPreInitConfig(
    const PreInitConfig& config)
{
    PreInitConfig viewConfig = config;
    viewConfig.hasSpacing = false;
    m_impl->storage.SetPreInitConfig(viewConfig);
}

void ViewPresentationState::SetTFNodes(
    const std::vector<TFNode>& nodes)
{
    m_impl->storage.SetTFNodes(nodes);
}

void ViewPresentationState::GetTFNodes(
    std::vector<TFNode>& destination) const
{
    m_impl->storage.GetTFNodes(destination);
}

void ViewPresentationState::SetTransferPresetIntent(
    const TransferPreset preset)
{
    m_impl->storage.SetTransferPresetIntent(preset);
}

bool ViewPresentationState::SetTransferPresetNodes(
    const TransferPreset preset,
    const DataVersion dataVersion,
    const std::vector<TFNode>& nodes)
{
    return m_impl->storage.SetTransferPresetNodes(
        preset, dataVersion, nodes);
}

TransferPreset ViewPresentationState::GetTransferPreset() const
{
    return m_impl->storage.GetTransferPreset();
}

void ViewPresentationState::SetIsoValue(const double value)
{
    m_impl->storage.SetIsoValue(value);
}

double ViewPresentationState::GetIsoValue() const
{
    return m_impl->storage.GetIsoValue();
}

void ViewPresentationState::SetMaterial(
    const MaterialParams& material)
{
    m_impl->storage.SetMaterial(material);
}

MaterialParams ViewPresentationState::GetMaterial() const
{
    return m_impl->storage.GetMaterial();
}

void ViewPresentationState::SetBackground(
    const BackgroundColor& background)
{
    m_impl->storage.SetBackground(background);
}

BackgroundColor ViewPresentationState::GetBackground() const
{
    return m_impl->storage.GetBackground();
}

void ViewPresentationState::SetWindowLevel(
    const double windowWidth,
    const double windowCenter)
{
    m_impl->storage.SetWindowLevel(windowWidth, windowCenter);
}

bool ViewPresentationState::SetAutoWindowLevel(
    const WindowLevelParams& windowLevel)
{
    return m_impl->storage.SetAutoWindowLevel(windowLevel);
}

bool ViewPresentationState::ResetWindowLevel(
    const WindowLevelParams& windowLevel)
{
    return m_impl->storage.ResetWindowLevel(windowLevel);
}

WindowLevelParams ViewPresentationState::GetWindowLevel() const
{
    return m_impl->storage.GetWindowLevel();
}

WindowLevelMode ViewPresentationState::GetWindowLevelMode() const
{
    return m_impl->storage.GetWindowLevelMode();
}

void ViewPresentationState::SetElementVisible(
    const uint32_t flagBit,
    const bool isVisible)
{
    m_impl->storage.SetElementVisible(flagBit, isVisible);
}

uint32_t ViewPresentationState::GetVisibilityMask() const
{
    return m_impl->storage.GetVisibilityMask();
}
