#pragma once
#include <vector>
#include <functional>
#include <memory>

// 定义控制节点结构
struct RenderNode {
    double position; // 0.0 - 1.0 (归一化位置)
    double opacity;  // 0.0 - 1.0
    double r, g, b;  // 颜色
};

// 定义观察者回调类型
using ObserverCallback = std::function<void()>;

// [新增] 内部结构体，保存“凭证”和“回调”
struct ObserverEntry {
    std::weak_ptr<void> owner;      // 存活凭证 (不增加引用计数)
    ObserverCallback callback; // 执行逻辑
};

class SharedInteractionState {
private:
    int m_cursorPosition[3] = { 0, 0, 0 };
    // 观察者列表：存放所有需要刷新的窗口的回调函数
    std::vector<ObserverEntry> m_observers;
    // --- 渲染状态数据 ---
    std::vector<TFNode> m_nodes;
    double m_dataRange[2] = { 0.0, 255.0 }; // 数据标量范围

public:
    SharedInteractionState(){
        // 初始化默认的4个节点
        // Point 0: Min
        m_nodes.push_back({ 0.0, 0.0, 0.0, 0.0, 0.0 });
        // Point 1: Low-Mid
        m_nodes.push_back({ 0.35, 0.0, 0.75, 0.75, 0.75 });
        // Point 2: High-Mid
        m_nodes.push_back({ 0.60, 0.6, 0.85, 0.85, 0.85 });
        // Point 3: Max
        m_nodes.push_back({ 1.0, 1.0, 0.95, 0.95, 0.95 });
    }

    // 设置数据范围 (用于将归一化节点映射到真实标量)
    void SetScalarRange(double min, double max) {
        m_dataRange[0] = min;
        m_dataRange[1] = max;
    }

    // 修改节点参数
    void SetNodeParameter(int index, double relativePos, double opacity) {
        if (index < 0 || index >= m_nodes.size()) return;
        m_nodes[index].position = relativePos;
        m_nodes[index].opacity = opacity;
        NotifyObservers();
    }

	// 获取节点列表
    const std::vector<TFNode>& GetTFNodes() const { return m_nodes; }
    const double* GetDataRange() const { return m_dataRange; }

    // 设置位置，并通知所有人
    void SetCursorPosition(int x, int y, int z) {
        if (m_cursorPosition[0] == x && m_cursorPosition[1] == y && m_cursorPosition[2] == z)
            return;

        m_cursorPosition[0] = x;
        m_cursorPosition[1] = y;
        m_cursorPosition[2] = z;

        NotifyObservers();
    }

    // 更新某个轴
    void UpdateAxis(int axisIndex, int delta, int maxDim) {
        m_cursorPosition[axisIndex] += delta;
        if (m_cursorPosition[axisIndex] < 0) m_cursorPosition[axisIndex] = 0;
        if (m_cursorPosition[axisIndex] >= maxDim) m_cursorPosition[axisIndex] = maxDim - 1;

        NotifyObservers();
    }

    int* GetCursorPosition() { return m_cursorPosition; }

    // 注册观察者
    void AddObserver(std::shared_ptr<void> owner, std::function<void()> cb) {
        if (!owner) return;
		m_observers.push_back({ owner, cb });
    }

private:
    void NotifyObservers() {
        for (auto it = m_observers.begin(); it != m_observers.end(); it++ ) {
            if (it->owner.expired())
            {
                m_observers.erase(it);
            }
            else
            {
                if (it->callback) it->callback();
            }
		}
    }
};
