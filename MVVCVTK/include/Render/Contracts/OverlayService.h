#pragma once

#include "Render/Contracts/FeatureOverlay.h"

#include <memory>

// Feature 只能经此窄接口增删叠加层，不能取得 App 具体对象。
class OverlayService {
public:
    virtual ~OverlayService() = default;

    virtual bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) = 0;
    // Remove 是清理屏障；实现即使遇到底层异常也必须撤销自身登记。
    virtual void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept = 0;
    virtual void ClearOverlays() noexcept = 0;
};
