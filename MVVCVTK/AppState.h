#pragma once
#include <vector>
#include <functional>
#include <memory>

// 定义一个简单的观察者回调类型
using ObserverCallback = std::function<void()>;

class SharedInteractionState {
private:
    int m_cursorPosition[3] = { 0, 0, 0 };
    // 观察者列表：存放所有需要刷新的窗口的回调函数
    std::vector<ObserverCallback> m_observers;

public:
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
    void AddObserver(ObserverCallback cb) {
        m_observers.push_back(cb);
    }

private:
    void NotifyObservers() {
        for (auto& cb : m_observers) {
            cb(); // 调用回调，通常是 renderWindow->Render()
        }
    }
};
