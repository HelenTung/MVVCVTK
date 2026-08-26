#pragma once

#include <memory>

class AbstractVisualStrategy;

// Feature 只能经此窄接口增删叠加层，不能取得 App 具体对象。
class OverlayService {
public:
    virtual ~OverlayService() = default;

    virtual void AttachOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) = 0;
    // Remove 是清理屏障；实现即使遇到底层异常也必须撤销自身登记。
    virtual void RemoveOverlayStrategy(
        std::shared_ptr<AbstractVisualStrategy> strategy) noexcept = 0;
    virtual void ClearOverlayStrategies() = 0;
};
