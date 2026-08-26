#pragma once

#include "Interaction/AbstractViewContext.h"
#include "Interaction/InteractionPorts.h"

#include <memory>

// 创建入口隐藏唯一的具体 context；Host 只取得抽象生命周期契约。
std::shared_ptr<AbstractViewContext> CreateViewContext(
    InteractionPorts ports);
