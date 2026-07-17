#include "App/AppState.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

// SharedInteractionState 的线程安全存储体：setter 在锁内比较并提交值，随后在锁外仅发布 UpdateFlags。
// 因而观察者拿到的是“哪些维度变化”的通知，实际值仍需通过本类 getter 读取一致快照。
class SharedInteractionState::Impl {
public:
    explicit Impl(std::shared_ptr<IStateEventSink> eventSink)
        : m_eventSink(std::move(eventSink))
        , m_nodes{
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 } }
    {
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

    void SendFlags(UpdateFlags flags)
    {
        std::shared_ptr<IStateEventSink> eventSink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            eventSink = m_eventSink;
        }
        // 外部观察者可能回读状态，因此必须在状态锁外广播。
        if (eventSink) eventSink->SendFlags(flags);
    }

    mutable std::mutex m_mutex;
    std::shared_ptr<IStateEventSink> m_eventSink;
    LoadState m_dataTrustedState = LoadState::Idle; // 当前可供渲染的数据是否可信；Reload 期间可继续为 Succeeded。
    LoadState m_fileLoadState = LoadState::Idle;    // 文件加载通道的最近状态。
    LoadState m_reloadLoadState = LoadState::Idle;  // 内存重载通道的最近状态。
    LoadEventKind m_activeLoadKind = LoadEventKind::None; // 全局 admission：File/Reload 同时最多一个在途事务。
    bool m_isLoadPublished = false;  // 终态广播已完成，owner 可以 ResetLoad 释放 admission。
    bool m_isLoadPublishing = false; // 广播窗口保护；防止回调重入提前重置当前事务。
    std::array<double, 2> m_dataRange = { 0.0, 255.0 }; // 当前标量 min/max，供 TF、ISO 与默认窗宽窗位使用。
    std::array<double, 3> m_spacing = { 1.0, 1.0, 1.0 }; // X/Y/Z 体素物理间距，单位 mm。
    std::vector<TFNode> m_nodes; // 按数据标量位置定义的颜色/不透明度传递函数控制点。
    double m_isoValue = 0.0; // 当前等值面阈值，单位与输入标量一致。
    MaterialParams m_material; // 主策略共享材质参数。
    BackgroundColor m_background; // renderer 共享背景色。
    WindowLevelParams m_windowLevel; // 2D slice 共享窗宽/窗位。
    uint32_t m_visibilityMask = VisFlags::Planes3D | VisFlags::Crosshair | VisFlags::Ruler; // overlay 可见位集合。
    bool m_isInteracting = false; // 交互期间策略可降低渲染质量/提高刷新率。
    std::array<double, 3> m_cursorWorld = { 0.0, 0.0, 0.0 }; // 约束到当前交互平面后的世界坐标。
    std::array<double, 3> m_cursorRawWorld = { 0.0, 0.0, 0.0 }; // 未约束的拾取世界坐标。
    int m_cursorAxis = -1; // 当前驱动切片的轴，-1 表示无特定轴；调用方负责传入合法轴域。
    std::array<double, 16> m_modelMatrix = { // 行主序 4x4 modelToWorld affine。
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
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
        m_impl->m_spacing = spacing;
        m_impl->m_fileLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->m_windowLevel = { rangeMax - rangeMin, (rangeMin + rangeMax) * 0.5 };
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
        m_impl->m_spacing = spacing;
        m_impl->m_reloadLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->m_windowLevel = { rangeMax - rangeMin, (rangeMin + rangeMax) * 0.5 };
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
        if (Impl::SetMaterial(m_impl->m_material, config.material)) flags |= UpdateFlags::Material;
        if (config.hasTF && Impl::SetTFNodes(m_impl->m_nodes, config.tfNodes)) flags |= UpdateFlags::TF;
        if (config.hasIso && Impl::SetScalar(m_impl->m_isoValue, config.isoThreshold)) flags |= UpdateFlags::IsoValue;
        if (config.hasBgColor && Impl::SetBackground(m_impl->m_background, config.bgColor)) flags |= UpdateFlags::Background;
        if (config.hasSpacing && Impl::SetArray(m_impl->m_spacing, config.spacing)) flags |= UpdateFlags::Spacing;
        if (config.hasWindowLevel && Impl::SetWindowLevel(m_impl->m_windowLevel, config.windowLevel)) flags |= UpdateFlags::WindowLevel;
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

std::array<double, 2> SharedInteractionState::GetDataRange() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_dataRange;
}

void SharedInteractionState::SetTFNodes(const std::vector<TFNode>& nodes)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetTFNodes(m_impl->m_nodes, nodes);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::TF);
}

void SharedInteractionState::GetTFNodes(std::vector<TFNode>& destination) const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    destination = m_impl->m_nodes;
}

void SharedInteractionState::SetIsoValue(double value)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetScalar(m_impl->m_isoValue, value);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::IsoValue);
}

double SharedInteractionState::GetIsoValue() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_isoValue;
}

void SharedInteractionState::SetMaterial(const MaterialParams& material)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetMaterial(m_impl->m_material, material);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Material);
}

MaterialParams SharedInteractionState::GetMaterial() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_material;
}

void SharedInteractionState::SetBackground(const BackgroundColor& background)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetBackground(m_impl->m_background, background);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Background);
}

BackgroundColor SharedInteractionState::GetBackground() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_background;
}

void SharedInteractionState::SetSpacing(
    double spacingX,
    double spacingY,
    double spacingZ)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(m_impl->m_spacing, { spacingX, spacingY, spacingZ });
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Spacing);
}

std::array<double, 3> SharedInteractionState::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_spacing;
}

void SharedInteractionState::SetWindowLevel(double windowWidth, double windowCenter)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetWindowLevel(
            m_impl->m_windowLevel, { windowWidth, windowCenter });
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::WindowLevel);
}

WindowLevelParams SharedInteractionState::GetWindowLevel() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_windowLevel;
}

void SharedInteractionState::SetInteracting(bool isInteracting)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetValue(m_impl->m_isInteracting, isInteracting);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Interaction);
}

bool SharedInteractionState::GetIsInteracting() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_isInteracting;
}

void SharedInteractionState::SetCursorWorld(double worldX, double worldY, double worldZ)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetArray(
            m_impl->m_cursorWorld, { worldX, worldY, worldZ }, 1e-9);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Cursor);
}

void SharedInteractionState::SetCursorRawWorld(
    double worldX,
    double worldY,
    double worldZ)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_cursorRawWorld = { worldX, worldY, worldZ };
}

std::array<double, 3> SharedInteractionState::GetCursorRawWorld() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_cursorRawWorld;
}

void SharedInteractionState::SetCursorAxis(int axis)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_cursorAxis = axis;
}

int SharedInteractionState::GetCursorAxis() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_cursorAxis;
}

std::array<double, 3> SharedInteractionState::GetCursorWorld() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_cursorWorld;
}

void SharedInteractionState::SetElementVisible(uint32_t flagBit, bool isVisible)
{
    bool hasChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        hasChanged = Impl::SetVisibilityMask(
            m_impl->m_visibilityMask, flagBit, isVisible);
    }
    if (hasChanged) m_impl->SendFlags(UpdateFlags::Visibility);
}

uint32_t SharedInteractionState::GetVisibilityMask() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_visibilityMask;
}
