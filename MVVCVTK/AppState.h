#pragma once
#include "AppTypes.h"
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <algorithm>
#include <cmath> 
#include <atomic>

// 观察者回调类型
using ObserverCallback = std::function<void(UpdateFlags)>;

struct ObserverEntry {
    std::weak_ptr<void> owner;    // 存活凭证（weak_ptr 不增加引用计数）
    ObserverCallback    callback;
};

class SharedInteractionState {
public:
    SharedInteractionState() {
        m_nodes = {
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 },
        };
    }

    // ── 数据就绪广播 ──────────────────────────────────────────────
    // 仅后台加载线程调用；写 range / spacing 后广播 DataReady
    void SetDataReady(double rangeMin, double rangeMax,
        const std::array<double, 3>& spacing) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange[0] = rangeMin;
            m_dataRange[1] = rangeMax;
            m_spacing = spacing;
            m_loadState = LoadState::Succeeded;
            m_windowLevel.windowWidth = rangeMax - rangeMin;
            m_windowLevel.windowCenter = (rangeMin + rangeMax) * 0.5;
        }
        SetObserversNotified(UpdateFlags::DataReady);
    }

    // ── 加载失败广播 ────────────────────────────────────────
    // 仅后台加载线程调用；设状态后广播 LoadFailed
    void SetLoadFailed() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_loadState = LoadState::Failed;
        }
        SetObserversNotified(UpdateFlags::LoadFailed);
    }

    // ── 加载状态 (LoadState 枚举，) ────────────────────────────
    void SetLoadState(LoadState s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_loadState = s;
    }
    LoadState GetLoadState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_loadState;
    }

    // ── 批量提交前处理配置（一次加锁 + 一次广播，精确 diff）────────
    // 对应 IVisualConfigService::CommitVisualConfig
    void SetPreInitConfig(const PreInitConfig& cfg) {
        UpdateFlags flags = UpdateFlags::None;
        {
            std::lock_guard<std::mutex> lk(m_mutex);

            // 材质逐字段比较，有变化才累加 Material 标志
            if (m_material.ambient != cfg.material.ambient ||
                m_material.diffuse != cfg.material.diffuse ||
                m_material.specular != cfg.material.specular ||
                m_material.specularPower != cfg.material.specularPower ||
                m_material.opacity != cfg.material.opacity ||
                m_material.shadeOn != cfg.material.shadeOn)
            {
                m_material = cfg.material;
                flags |= UpdateFlags::Material;
            }

            // 传输函数
            if (cfg.hasTF) {
                m_nodes = cfg.tfNodes;
                flags |= UpdateFlags::TF;
            }

            // 等值面阈值
            if (cfg.hasIso && std::abs(m_isoValue - cfg.isoThreshold) > 1e-6) {
                m_isoValue = cfg.isoThreshold;
                flags |= UpdateFlags::IsoValue;
            }

            // 背景色
            if (cfg.hasBgColor &&
                (std::abs(m_background.r - cfg.bgColor.r) > 1e-6 ||
                    std::abs(m_background.g - cfg.bgColor.g) > 1e-6 ||
                    std::abs(m_background.b - cfg.bgColor.b) > 1e-6))
            {
                m_background = cfg.bgColor;
                flags |= UpdateFlags::Background;
            }

            // spacing
            if (cfg.hasSpacing &&
                (std::abs(m_spacing[0] - cfg.spacing[0]) > 1e-6 ||
                    std::abs(m_spacing[1] - cfg.spacing[1]) > 1e-6 ||
                    std::abs(m_spacing[2] - cfg.spacing[2]) > 1e-6))
            {
                m_spacing = cfg.spacing;
                flags |= UpdateFlags::Spacing;
            }

			// 切片窗宽/窗位
            if (cfg.hasWindowLevel &&
                (std::abs(m_windowLevel.windowWidth - cfg.windowLevel.windowWidth) > 1e-6 ||
                    std::abs(m_windowLevel.windowCenter - cfg.windowLevel.windowCenter) > 1e-6))
            {
                m_windowLevel = cfg.windowLevel;
                flags |= UpdateFlags::WindowLevel;
            }
        }
        if (flags != UpdateFlags::None)
            SetObserversNotified(flags);
    }

    // ── 模型变换矩阵 ──────────────────────────────────────────────
    void SetModelMatrix(const std::array<double, 16>& mat) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_modelMatrix = mat;
        }
        SetObserversNotified(UpdateFlags::Transform);
    }
    std::array<double, 16> GetModelMatrix() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_modelMatrix;
    }

    // ── 标量范围 ──────────────────────────────────────────────────
    void SetScalarRange(double minv, double maxv) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange[0] = minv;
            m_dataRange[1] = maxv;
        }
        SetObserversNotified(UpdateFlags::TF);
    }
    std::array<double, 2> GetDataRange() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return { m_dataRange[0], m_dataRange[1] };
    }

    // ── 传输函数节点 ──────────────────────────────────────────────
    void SetTFNodes(const std::vector<TFNode>& nodes) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_nodes.size() != nodes.size()) {
                m_nodes = nodes;
                changed = true;
            }
            else {
                const bool same = std::equal(m_nodes.begin(), m_nodes.end(), nodes.begin(),
                    [](const TFNode& a, const TFNode& b) {
                        return std::abs(a.position - b.position) <= 1e-6 &&
                            std::abs(a.opacity - b.opacity) <= 1e-6 &&
                            std::abs(a.r - b.r) <= 1e-6 &&
                            std::abs(a.g - b.g) <= 1e-6 &&
                            std::abs(a.b - b.b) <= 1e-6;
                    });
                if (!same) {
                    m_nodes = nodes;
                    changed = true;
                }
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::TF);
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
            if (std::abs(m_isoValue - val) > 1e-6) {
                m_isoValue = val;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::IsoValue);
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
            if (std::abs(m_material.ambient - mat.ambient) > 1e-6 ||
                std::abs(m_material.diffuse - mat.diffuse) > 1e-6 ||
                std::abs(m_material.specular - mat.specular) > 1e-6 ||
                std::abs(m_material.specularPower - mat.specularPower) > 1e-6 ||
                std::abs(m_material.opacity - mat.opacity) > 1e-6 ||
                m_material.shadeOn != mat.shadeOn)
            {
                m_material = mat;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::Material);
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
            if (std::abs(m_background.r - bg.r) > 1e-6 ||
                std::abs(m_background.g - bg.g) > 1e-6 ||
                std::abs(m_background.b - bg.b) > 1e-6)
            {
                m_background = bg;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::Background);
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
            if (std::abs(m_spacing[0] - sx) > 1e-6 ||
                std::abs(m_spacing[1] - sy) > 1e-6 ||
                std::abs(m_spacing[2] - sz) > 1e-6)
            {
                m_spacing = { sx, sy, sz };
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::Spacing);
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
            if (std::abs(m_windowLevel.windowWidth - ww) > 1e-6 ||
                std::abs(m_windowLevel.windowCenter - wc) > 1e-6)
            {
                m_windowLevel.windowWidth = ww;
                m_windowLevel.windowCenter = wc;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::WindowLevel);
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
            if (m_isInteracting != val) {
                m_isInteracting = val;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::Interaction);
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
            const double eps = 1e-9;
            if (std::abs(m_cursorWorld[0] - x) > eps ||
                std::abs(m_cursorWorld[1] - y) > eps ||
                std::abs(m_cursorWorld[2] - z) > eps) {
                m_cursorWorld[0] = x;
                m_cursorWorld[1] = y;
                m_cursorWorld[2] = z;
                changed = true;
            }
        }
        if (changed) SetObserversNotified(UpdateFlags::Cursor);
    }

    void SetCursorRawWorld(double x, double y, double z) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cursorRawWorld[0] = x;
        m_cursorRawWorld[1] = y;
        m_cursorRawWorld[2] = z;
    }

    std::array<double, 3> GetCursorRawWorld() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return { m_cursorRawWorld[0], m_cursorRawWorld[1], m_cursorRawWorld[2] };
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
        return { m_cursorWorld[0], m_cursorWorld[1], m_cursorWorld[2] };
    }

    // ── 显隐状态 ──────────────────────────────────────────────────────────
    void SetElementVisible(uint32_t flagBit, bool show) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            const uint32_t oldMask = m_visibilityMask;
            if (show)
                m_visibilityMask |= flagBit;
            else
                m_visibilityMask &= ~flagBit;
            changed = (m_visibilityMask != oldMask);
        }
        if (changed) SetObserversNotified(UpdateFlags::Visibility);
    }

    uint32_t GetVisibilityMask() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_visibilityMask;
    }

    // ── Observer 管理 ─────────────────────────────────────────────
    void SetObserver(std::shared_ptr<void> owner, ObserverCallback cb) {
        if (!owner || !cb) return;
        std::lock_guard<std::mutex> lk(m_mutex);
		// 先清理再放入，保持列表干净（不保留过期条目）
        // 清理已过期条目，避免列表膨胀，"erase-remove 惯用法"
        m_observers.erase(
            // vec.erase(std::remove_if(vec.begin(), vec.end(), 条件谓词), vec.end());
			// 条件谓词返回 true 的元素会被移除，返回 false 的元素会被保留
            // 把谓词理解成 "是垃圾吗？"
            std::remove_if(m_observers.begin(), m_observers.end(),
                [](const ObserverEntry& e) { return e.owner.expired(); }),
            m_observers.end());
        m_observers.push_back({ std::move(owner), std::move(cb) });
    }

private:
    mutable std::mutex m_mutex;

    // 数据生命周期状态
    LoadState m_loadState = LoadState::Idle;                    // 当前加载状态（Idle / Loading / Succeeded / Failed）
    double m_dataRange[2] = { 0.0, 255.0 };                    // 当前体数据标量范围
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
    double m_cursorWorld[3] = { 0, 0, 0 };                      // 当前联动光标世界坐标
    double m_cursorRawWorld[3] = { 0, 0, 0 };                   // 原始拾取得到的世界坐标
    int m_cursorAxis = -1;                                      // 当前光标来源轴（-1 表示自由点）

    // 模型状态
    std::array<double, 16> m_modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };                                                         // 当前模型矩阵（列主序 4x4）

    // 观察者
    std::vector<ObserverEntry> m_observers;                     // 当前已注册的观察者列表

    // ── NotifyObservers：先快照回调列表（持锁），再无锁调用 ──
    // 彻底消除回调中调用 Set* → 重入加锁 → 死锁风险
    void SetObserversNotified(UpdateFlags flags) {
        // 持锁扫描，快照存活回调并清理过期条目
        std::vector<ObserverCallback> toRun;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto it = m_observers.begin(); it != m_observers.end(); ) {
                if (it->owner.expired()) {
                    it = m_observers.erase(it);
                }
                else {
                    toRun.push_back(it->callback);
                    ++it;
                }
            }
        }
        // 无锁调用（允许回调中再次调用 Set*，不再死锁）
        for (auto& cb : toRun)
            if (cb) cb(flags);
    }
};