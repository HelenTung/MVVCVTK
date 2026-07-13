#include "App/AppState.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

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
    LoadState m_dataTrustedState = LoadState::Idle;
    LoadState m_fileLoadState = LoadState::Idle;
    LoadState m_reloadLoadState = LoadState::Idle;
    LoadEventKind m_activeLoadKind = LoadEventKind::None;
    bool m_isLoadPublished = false;
    std::array<double, 2> m_dataRange = { 0.0, 255.0 };
    std::array<double, 3> m_spacing = { 1.0, 1.0, 1.0 };
    std::vector<TFNode> m_nodes;
    double m_isoValue = 0.0;
    MaterialParams m_material;
    BackgroundColor m_background;
    WindowLevelParams m_windowLevel;
    uint32_t m_visibilityMask = VisFlags::Planes3D | VisFlags::Crosshair | VisFlags::Ruler;
    bool m_isInteracting = false;
    std::array<double, 3> m_cursorWorld = { 0.0, 0.0, 0.0 };
    std::array<double, 3> m_cursorRawWorld = { 0.0, 0.0, 0.0 };
    int m_cursorAxis = -1;
    std::array<double, 16> m_modelMatrix = {
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
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if ((loadEventKind != LoadEventKind::File
        && loadEventKind != LoadEventKind::Reload)
        || m_impl->m_activeLoadKind != LoadEventKind::None) {
        return false;
    }

    m_impl->m_activeLoadKind = loadEventKind;
    m_impl->m_isLoadPublished = false;
    if (loadEventKind == LoadEventKind::File) {
        m_impl->m_fileLoadState = LoadState::Loading;
        m_impl->m_dataTrustedState = LoadState::Loading;
    }
    else {
        m_impl->m_reloadLoadState = LoadState::Loading;
    }
    return true;
}

bool SharedInteractionState::ResetLoad(LoadEventKind loadEventKind)
{
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if (loadEventKind == LoadEventKind::None
        || m_impl->m_activeLoadKind != loadEventKind) {
        return false;
    }
    const bool isLoading = loadEventKind == LoadEventKind::File
        ? m_impl->m_fileLoadState == LoadState::Loading
        : m_impl->m_reloadLoadState == LoadState::Loading;
    if (!isLoading && !m_impl->m_isLoadPublished) {
        return false;
    }
    m_impl->m_activeLoadKind = LoadEventKind::None;
    m_impl->m_isLoadPublished = false;
    return true;
}

void SharedInteractionState::SetFileDataReady(
    double rangeMin,
    double rangeMax,
    const std::array<double, 3>& spacing)
{
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_dataRange = { rangeMin, rangeMax };
        m_impl->m_spacing = spacing;
        m_impl->m_fileLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->m_windowLevel = { rangeMax - rangeMin, (rangeMin + rangeMax) * 0.5 };
    }
    m_impl->SendFlags(UpdateFlags::DataReady | UpdateFlags::FileLoad);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::File) {
            m_impl->m_isLoadPublished = true;
        }
    }
}

void SharedInteractionState::SetReloadDataReady(
    double rangeMin,
    double rangeMax,
    const std::array<double, 3>& spacing)
{
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_dataRange = { rangeMin, rangeMax };
        m_impl->m_spacing = spacing;
        m_impl->m_reloadLoadState = LoadState::Succeeded;
        m_impl->m_dataTrustedState = LoadState::Succeeded;
        m_impl->m_windowLevel = { rangeMax - rangeMin, (rangeMin + rangeMax) * 0.5 };
    }
    m_impl->SendFlags(UpdateFlags::DataReady | UpdateFlags::ReloadLoad);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::Reload) {
            m_impl->m_isLoadPublished = true;
        }
    }
}

void SharedInteractionState::SetFileLoadFailed()
{
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_fileLoadState = LoadState::Failed;
        m_impl->m_dataTrustedState = LoadState::Failed;
    }
    m_impl->SendFlags(UpdateFlags::LoadFailed | UpdateFlags::FileLoad);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::File) {
            m_impl->m_isLoadPublished = true;
        }
    }
}

void SharedInteractionState::SetReloadLoadFailed()
{
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_reloadLoadState = LoadState::Failed;
        if (m_impl->m_dataTrustedState != LoadState::Succeeded) {
            m_impl->m_dataTrustedState = LoadState::Failed;
        }
    }
    m_impl->SendFlags(UpdateFlags::LoadFailed | UpdateFlags::ReloadLoad);
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        if (m_impl->m_activeLoadKind == LoadEventKind::Reload) {
            m_impl->m_isLoadPublished = true;
        }
    }
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
