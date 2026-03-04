#pragma once
#include "AppTypes.h"
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
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
    // 仅后台加载线程调用；写 range 后广播 DataReady
    void NotifyDataReady(double rangeMin, double rangeMax) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange[0] = rangeMin;
            m_dataRange[1] = rangeMax;
            m_loadState = LoadState::Succeeded;
        }
        NotifyObservers(UpdateFlags::DataReady);
    }

    // ── 加载失败广播 ────────────────────────────────────────
    // 仅后台加载线程调用；设状态后广播 LoadFailed
    void NotifyLoadFailed() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_loadState = LoadState::Failed;
        }
        NotifyObservers(UpdateFlags::LoadFailed);
    }

    // ── 加载状态 (LoadState 枚举，NEW) ────────────────────────────
    void SetLoadState(LoadState s) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_loadState = s;
    }
    LoadState GetLoadState() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_loadState;
    }

    // ── 批量提交前处理配置（一次加锁 + 一次广播，精确 diff）────────
    // 对应 IPreInitService::PreInit_CommitConfig
    void CommitPreInitConfig(const PreInitConfig& cfg) {
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

            // 背景色（NEW）
            if (cfg.hasBgColor &&
                (std::abs(m_background.r - cfg.bgColor.r) > 1e-6 ||
                    std::abs(m_background.g - cfg.bgColor.g) > 1e-6 ||
                    std::abs(m_background.b - cfg.bgColor.b) > 1e-6))
            {
                m_background = cfg.bgColor;
                flags |= UpdateFlags::Background;
            }
        }
        if (flags != UpdateFlags::None)
            NotifyObservers(flags);
    }

    // ── 模型变换矩阵 ──────────────────────────────────────────────
    void SetModelMatrix(const std::array<double, 16>& mat) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_modelMatrix = mat;
        }
        NotifyObservers(UpdateFlags::Transform);
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
        NotifyObservers(UpdateFlags::TF);
    }
    std::array<double, 2> GetDataRange() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return { m_dataRange[0], m_dataRange[1] };
    }

    // ── 传输函数节点 ──────────────────────────────────────────────
    void SetTFNodes(const std::vector<TFNode>& nodes) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_nodes = nodes;
        }
        NotifyObservers(UpdateFlags::TF);
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
        if (changed) NotifyObservers(UpdateFlags::IsoValue);
    }
    double GetIsoValue() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_isoValue;
    }

    // ── 材质参数 ──────────────────────────────────────────────────
    void SetMaterial(const MaterialParams& mat) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_material = mat;
        }
        NotifyObservers(UpdateFlags::Material);
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
        if (changed) NotifyObservers(UpdateFlags::Background);
    }
    BackgroundColor GetBackground() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_background;
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
        if (changed) NotifyObservers(UpdateFlags::Interaction);
    }
    bool IsInteracting() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_isInteracting;
    }

    // ── 光标位置 ──────────────────────────────────────────────────
    void SetCursorPosition(int x, int y, int z) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_cursorPos[0] != x || m_cursorPos[1] != y || m_cursorPos[2] != z) {
                m_cursorPos[0] = x;
                m_cursorPos[1] = y;
                m_cursorPos[2] = z;
                changed = true;
            }
        }
        if (changed) NotifyObservers(UpdateFlags::Cursor);
    }
    void UpdateAxis(int axisIndex, int delta, int maxDim) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            int nv = m_cursorPos[axisIndex] + delta;
            nv = nv < 0 ? 0 : (nv >= maxDim ? maxDim - 1 : nv);
            if (m_cursorPos[axisIndex] != nv) {
                m_cursorPos[axisIndex] = nv;
                changed = true;
            }
        }
        if (changed) NotifyObservers(UpdateFlags::Cursor);
    }
    std::array<int, 3> GetCursorPosition() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return { m_cursorPos[0], m_cursorPos[1], m_cursorPos[2] };
    }

    // ── Observer 管理 ─────────────────────────────────────────────
    void AddObserver(std::shared_ptr<void> owner, ObserverCallback cb) {
        if (!owner || !cb) return;
        std::lock_guard<std::mutex> lk(m_mutex);
        // 清理已过期条目，避免列表膨胀
        m_observers.erase(
            std::remove_if(m_observers.begin(), m_observers.end(),
                [](const ObserverEntry& e) { return e.owner.expired(); }),
            m_observers.end());
        m_observers.push_back({ std::move(owner), std::move(cb) });
    }

private:
    mutable std::mutex m_mutex;

    int                    m_cursorPos[3] = { 0, 0, 0 };
    double                 m_isoValue = 0.0;
    MaterialParams         m_material;
    BackgroundColor        m_background;                   // ← NEW
    std::vector<TFNode>    m_nodes;
    double                 m_dataRange[2] = { 0.0, 255.0 };
    bool                   m_isInteracting = false;
    LoadState              m_loadState = LoadState::Idle; // ← NEW
    std::array<double, 16> m_modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    std::vector<ObserverEntry> m_observers;

    // ── NotifyObservers：先快照回调列表（持锁），再无锁调用 ──
    // 彻底消除回调中调用 Set* → 重入加锁 → 死锁风险
    void NotifyObservers(UpdateFlags flags) {
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