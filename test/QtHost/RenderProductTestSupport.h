#pragma once

#include "Render/Support/BaseVisualStrategy.h"
#include "Render/Internal/RenderResourceCoordinator.h"

#include <vtkDataObject.h>
#include <vtkSmartPointer.h>

#include <deque>
#include <utility>

class RenderProductStrategyProbe final : public BaseVisualStrategy {
public:
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
};

class ManualRenderLane final {
public:
    bool Start(RenderLaneWork work)
    {
        if (!m_isAccepting || !work.valid()) {
            return false;
        }
        m_work.push_back(std::move(work));
        ++m_startCount;
        return true;
    }

    bool SendOne()
    {
        if (m_work.empty()) return false;
        auto work = std::move(m_work.front());
        m_work.pop_front();
        TaskStopSource stopSource;
        work(stopSource.GetToken());
        return true;
    }

    void SetAccepting(const bool isAccepting) noexcept
    {
        m_isAccepting = isAccepting;
    }

    std::size_t GetPendingCount() const noexcept
    {
        return m_work.size();
    }

    std::size_t GetStartCount() const noexcept
    {
        return m_startCount;
    }

private:
    std::deque<RenderLaneWork> m_work;
    std::size_t m_startCount = 0;
    bool m_isAccepting = true;
};
