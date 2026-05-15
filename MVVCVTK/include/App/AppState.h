#pragma once
#include "AppTypes.h"
#include "AppStateCommands.h"
#include "AppStateEvents.h"
#include <vector>
#include <memory>
#include <mutex>
#include <array>
#include <algorithm>
#include <cmath>
#include <atomic>

class SharedInteractionState {
public:
    explicit SharedInteractionState(std::shared_ptr<IStateEventSink> eventSink = nullptr)
        : m_eventSink(std::move(eventSink)) {
        m_nodes = {
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 },
        };
    }

    void SetEventSink(std::shared_ptr<IStateEventSink> eventSink) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_eventSink = std::move(eventSink);
    }

    // ── 文件流加载状态 ────────────────────────────────────────────
    void SetFileLoadState(LoadState s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_fileLoadState = s;
    }
    LoadState GetFileLoadState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_fileLoadState;
    }

    // ── 重载加载状态 ──────────────────────────────────────────────
    void SetReloadLoadState(LoadState s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_reloadLoadState = s;
    }
    LoadState GetReloadLoadState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_reloadLoadState;
    }

    // ── 数据可信状态 ──────────────────────────────────────────────
    void SetDataTrustedState(LoadState s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_dataTrustedState = s;
    }
    LoadState GetDataTrustedState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_dataTrustedState;
    }

    // ── 文件流加载开始 ────────────────────────────────────────────
    void SetFileLoadStarted() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_fileLoadState = LoadState::Loading;
        m_dataTrustedState = LoadState::Loading;
    }

    // ── 重载加载开始 ──────────────────────────────────────────────
    void SetReloadLoadStarted() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_reloadLoadState = LoadState::Loading;
    }

    // ── 文件流数据就绪广播 ────────────────────────────────────────
    // 仅后台文件加载线程调用；写 range / spacing 后广播 DataReady
    void SetFileDataReady(double rangeMin, double rangeMax,
        const std::array<double, 3>& spacing) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange = { rangeMin, rangeMax };
            m_spacing = spacing;
            m_fileLoadState = LoadState::Succeeded;
            m_dataTrustedState = LoadState::Succeeded;
            m_windowLevel.windowWidth = rangeMax - rangeMin;
            m_windowLevel.windowCenter = (rangeMin + rangeMax) * 0.5;
        }
        SetFlagsPublished(UpdateFlags::DataReady);
    }

    // ── 重载数据就绪广播 ──────────────────────────────────────────
    // 仅后台重载主线程提交调用；写 range / spacing 后广播 DataReady
    void SetReloadDataReady(double rangeMin, double rangeMax,
        const std::array<double, 3>& spacing) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange = { rangeMin, rangeMax };
            m_spacing = spacing;
            m_reloadLoadState = LoadState::Succeeded;
            m_dataTrustedState = LoadState::Succeeded;
            m_windowLevel.windowWidth = rangeMax - rangeMin;
            m_windowLevel.windowCenter = (rangeMin + rangeMax) * 0.5;
        }
        SetFlagsPublished(UpdateFlags::DataReady);
    }

    // ── 文件流加载失败广播 ────────────────────────────────────────
    // 仅后台文件加载线程调用；设状态后广播 LoadFailed
    void SetFileLoadFailed() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_fileLoadState = LoadState::Failed;
            m_dataTrustedState = LoadState::Failed;
        }
        SetFlagsPublished(UpdateFlags::LoadFailed);
    }

    // ── 重载加载失败广播 ──────────────────────────────────────────
    // 仅后台重载线程调用；设状态后广播 LoadFailed
    void SetReloadLoadFailed() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_reloadLoadState = LoadState::Failed;
            if (m_dataTrustedState != LoadState::Succeeded) {
                m_dataTrustedState = LoadState::Failed;
            }
        }
        SetFlagsPublished(UpdateFlags::LoadFailed);
    }

    // ── 聚合加载状态（兼容现有接口） ─────────────────────────────
    LoadState GetLoadState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_fileLoadState == LoadState::Loading
            || m_reloadLoadState == LoadState::Loading
            || m_dataTrustedState == LoadState::Loading)
        {
            return LoadState::Loading;
        }
        if (m_dataTrustedState == LoadState::Succeeded) {
            return LoadState::Succeeded;
        }
        if (m_fileLoadState == LoadState::Failed
            || m_reloadLoadState == LoadState::Failed
            || m_dataTrustedState == LoadState::Failed)
        {
            return LoadState::Failed;
        }
        if (m_fileLoadState == LoadState::Succeeded
            || m_reloadLoadState == LoadState::Succeeded)
        {
            return LoadState::Succeeded;
        }
        return LoadState::Idle;
    }

    // ── 批量提交前处理配置（一次加锁 + 一次广播，精确 diff）────────
    // 对应 IVisualConfigService::CommitVisualConfig
    void SetPreInitConfig(const PreInitConfig& cfg) {
        UpdateFlags flags = UpdateFlags::None;
        {
            std::lock_guard<std::mutex> lk(m_mutex);

            if (AppStateCommands::SetMaterial(m_material, cfg.material)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::Material);
            }

            if (cfg.hasTF && AppStateCommands::SetTFNodes(m_nodes, cfg.tfNodes)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::TF);
            }

            if (cfg.hasIso && AppStateCommands::SetScalar(m_isoValue, cfg.isoThreshold)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::IsoValue);
            }

            if (cfg.hasBgColor && AppStateCommands::SetBackground(m_background, cfg.bgColor)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::Background);
            }

            if (cfg.hasSpacing && AppStateCommands::SetArray(m_spacing, cfg.spacing)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::Spacing);
            }

            if (cfg.hasWindowLevel && AppStateCommands::SetWindowLevel(m_windowLevel, cfg.windowLevel)) {
                AppStateCommands::SetFlagsMerged(flags, UpdateFlags::WindowLevel);
            }
        }
        if (flags != UpdateFlags::None)
            SetFlagsPublished(flags);
    }

    // ── 模型变换矩阵 ──────────────────────────────────────────────
    void SetModelMatrix(const std::array<double, 16>& mat) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetArray(m_modelMatrix, mat, 1e-9);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Transform);
    }
    std::array<double, 16> GetModelMatrix() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_modelMatrix;
    }

    // ── 标量范围 ──────────────────────────────────────────────────
    void SetScalarRange(double minv, double maxv) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetArray(m_dataRange, { minv, maxv });
        }
        if (changed) SetFlagsPublished(UpdateFlags::TF);
    }
    std::array<double, 2> GetDataRange() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_dataRange;
    }

    // ── 传输函数节点 ──────────────────────────────────────────────
    void SetTFNodes(const std::vector<TFNode>& nodes) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetTFNodes(m_nodes, nodes);
        }
        if (changed) SetFlagsPublished(UpdateFlags::TF);
    }
    void GetTFNodes(std::vector<TFNode>& dest) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        dest = m_nodes;
    }

    // ── 等值面阈值 ────────────────────────────────────────────────
    void SetIsoValue(double val) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetScalar(m_isoValue, val);
        }
        if (changed) SetFlagsPublished(UpdateFlags::IsoValue);
    }
    double GetIsoValue() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_isoValue;
    }

    // ── 材质参数 ──────────────────────────────────────────────────
    void SetMaterial(const MaterialParams& mat) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetMaterial(m_material, mat);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Material);
    }
    MaterialParams GetMaterial() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_material;
    }

    // ── 背景色 ──────────────────────────────────────────────
    void SetBackground(const BackgroundColor& bg) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetBackground(m_background, bg);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Background);
    }
    BackgroundColor GetBackground() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_background;
    }

    // ── 体数据 spacing ──────────────────────────────────────────────
    void SetSpacing(double sx, double sy, double sz) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetArray(m_spacing, { sx, sy, sz });
        }
        if (changed) SetFlagsPublished(UpdateFlags::Spacing);
    }

    std::array<double, 3> GetSpacing() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_spacing;
    }

    // ── 切片窗宽/窗位（WW/WC，工业 CT 标准）─────────────────────
    void SetWindowLevel(double ww, double wc) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetWindowLevel(m_windowLevel, { ww, wc });
        }
        if (changed) SetFlagsPublished(UpdateFlags::WindowLevel);
    }
    WindowLevelParams GetWindowLevel() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_windowLevel;
    }


    // ── 交互状态 ──────────────────────────────────────────────────
    void SetInteracting(bool val) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetValue(m_isInteracting, val);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Interaction);
    }
    bool GetIsInteracting() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_isInteracting;
    }

    // ── 光标位置 ──────────────────────────────────────────────────
    void SetCursorWorld(double x, double y, double z) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetArray(m_cursorWorld, { x, y, z }, 1e-9);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Cursor);
    }

    void SetCursorRawWorld(double x, double y, double z) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cursorRawWorld = { x, y, z };
    }

    std::array<double, 3> GetCursorRawWorld() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_cursorRawWorld;
    }

    void SetCursorAxis(int axis) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cursorAxis = axis;
    }

    int GetCursorAxis() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_cursorAxis;
    }

    std::array<double, 3> GetCursorWorld() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_cursorWorld;
    }

    // ── 显隐状态 ──────────────────────────────────────────────────────────
    void SetElementVisible(uint32_t flagBit, bool show) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            changed = AppStateCommands::SetVisibilityMask(m_visibilityMask, flagBit, show);
        }
        if (changed) SetFlagsPublished(UpdateFlags::Visibility);
    }

    uint32_t GetVisibilityMask() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_visibilityMask;
    }

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<IStateEventSink> m_eventSink;

    // 数据生命周期状态
    LoadState m_dataTrustedState = LoadState::Idle;            // 当前数据可信状态（Idle / Loading / Succeeded / Failed）
    LoadState m_fileLoadState = LoadState::Idle;               // 当前文件流加载状态（Idle / Loading / Succeeded / Failed）
    LoadState m_reloadLoadState = LoadState::Idle;             // 当前重载加载状态（Idle / Loading / Succeeded / Failed）
    std::array<double, 2> m_dataRange = { 0.0, 255.0 };        // 当前体数据标量范围
    std::array<double, 3> m_spacing = { 1.0, 1.0, 1.0 };       // 当前体数据 spacing（RAS 世界坐标系）

    // 渲染配置状态
    std::vector<TFNode> m_nodes;                                // 当前体渲染传输函数节点
    double m_isoValue = 0.0;                                    // 当前等值面阈值
    MaterialParams m_material;                                  // 当前材质参数
    BackgroundColor m_background;                               // 当前背景色
    WindowLevelParams m_windowLevel;                            // 当前切片窗宽/窗位
    uint32_t m_visibilityMask = VisFlags::Planes3D
        | VisFlags::Crosshair
        | VisFlags::Ruler;                                      // 当前辅助元素可见性掩码

    // 交互状态
    bool m_isInteracting = false;                               // 当前是否处于高频交互中
    std::array<double, 3> m_cursorWorld = { 0.0, 0.0, 0.0 };    // 当前联动光标世界坐标
    std::array<double, 3> m_cursorRawWorld = { 0.0, 0.0, 0.0 }; // 原始拾取得到的世界坐标
    int m_cursorAxis = -1;                                      // 当前光标来源轴（-1 表示自由点）

    // 模型状态
    std::array<double, 16> m_modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };                                                         // 当前模型矩阵（列主序 4x4）

    void SetFlagsPublished(UpdateFlags flags) {
        std::shared_ptr<IStateEventSink> eventSink;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            eventSink = m_eventSink;
        }

        if (eventSink) {
            eventSink->SetFlagsPublished(flags);
        }
    }
};
