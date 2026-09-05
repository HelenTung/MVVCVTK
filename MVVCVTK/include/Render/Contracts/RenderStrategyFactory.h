#pragma once

#include "App/AppTypes.h"

#include <functional>
#include <memory>

class AbstractVisualStrategy;
struct RenderStrategyServices;

using StrategyCreate =
    std::function<std::shared_ptr<AbstractVisualStrategy>(VizMode)>;

// Render 层唯一的 mode -> concrete Strategy 组合入口。
std::shared_ptr<AbstractVisualStrategy> CreateRenderStrategy(
    VizMode mode);
std::shared_ptr<AbstractVisualStrategy> CreateRenderStrategy(
    VizMode mode,
    const std::shared_ptr<RenderStrategyServices>& services);
