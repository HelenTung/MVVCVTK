#pragma once
// =====================================================================
// AppState.h — SharedInteractionState（共享交互状态中枢）
//
// 依赖：AppTypes.h（UpdateFlags / TFNode / MaterialParams）
//
// 线程安全：所有 Set*/Get*/Notify* 均通过 m_mutex 保护。
// =====================================================================

#include "AppTypes.h"
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include <cmath>

// 观察者回调类型
using ObserverCallback = std::function<void(UpdateFlags)>;

struct ObserverEntry {
    std::weak_ptr<void> owner;     // 存活凭证（不增加引用计数）
    ObserverCallback    callback;
};

class SharedInteractionState {
public:
    SharedInteractionState() {
        // 默认 4 个传输函数节点
        m_nodes = {
            { 0.00, 0.0, 0.00, 0.00, 0.00 },
            { 0.35, 0.0, 0.75, 0.75, 0.75 },
            { 0.60, 0.6, 0.85, 0.85, 0.85 },
            { 1.00, 1.0, 0.95, 0.95, 0.95 },
        };
    }

    // ── 数据就绪广播 ──────────────────────────────────────────────
    void NotifyDataReady(double rangeMin, double rangeMax) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_dataRange[0] = rangeMin;
            m_dataRange[1] = rangeMax;
        }
        NotifyObservers(UpdateFlags::DataReady);
    }

    // ── 批量提交前处理配置（仅一次加锁 + 一次广播）───────────────
    // 对应 IPreInitService::PreInit_CommitConfig
    void CommitPreInitConfig(const PreInitConfig& cfg) {
        UpdateFlags flags = UpdateFlags::None;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_material = cfg.material;
            flags = flags | UpdateFlags::Material;
            if (cfg.hasTF) {
                m_nodes = cfg.tfNodes;
                flags = flags | UpdateFlags::TF;
            }
            if (cfg.hasIso && std::abs(m_isoValue - cfg.isoThreshold) > 0.0001) {
                m_isoValue = cfg.isoThreshold;
                flags = flags | UpdateFlags::IsoValue;
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
            if (std::abs(m_isoValue - val) > 0.0001) {
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

    // ── 材质参数 ─────��────────────────────────────────────────────
    void SetMaterial(const MaterialParams& mat) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_material = mat;
        }
        NotifyObservers(UpdateFlags::Material);
    }
    // 返回值拷贝而非 const&，避免未加锁的数据竞争
    MaterialParams GetMaterial() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_material;
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
        std::lock_guard<std::mutex> lk(m_mutex);
        if (owner) m_observers.push_back({ std::move(owner), std::move(cb) });
    }

private:
    mutable std::mutex m_mutex;

    int                    m_cursorPos[3] = { 0, 0, 0 };
    double                 m_isoValue = 0.0;
    MaterialParams         m_material;
    std::vector<TFNode>    m_nodes;
    double                 m_dataRange[2] = { 0.0, 255.0 };
    bool                   m_isInteracting = false;
    std::array<double, 16> m_modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    std::vector<ObserverEntry> m_observers;

    void NotifyObservers(UpdateFlags flags) {
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
        for (auto& cb : toRun)
            if (cb) cb(flags);
    }
};