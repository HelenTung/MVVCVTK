#pragma once
#include "IsoSurfaceStrategy.h"
#include "VolumeStrategy.h"
#include "SliceStrategy.h"
#include "MultiSliceStrategy.h"
#include "ColoredPlanesStrategy.h"
#include "CompositeStrategy.h"
#include "AppInterfaces.h"
// 工厂引用所有策略

class StrategyFactory {
public:
    static std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode) {
        // VizMode 是上层唯一关心的渲染模式标识；
        // 工厂负责把它映射到具体 Strategy 类型，避免 Service 层直接依赖各个策略类。
        switch (mode) {
        case VizMode::Volume: return std::make_shared<VolumeStrategy>();
        case VizMode::IsoSurface: return std::make_shared<IsoSurfaceStrategy>();
        case VizMode::SliceTop_down: return std::make_shared<SliceStrategy>(Orientation::Top_down);
        case VizMode::SliceFront_back: return std::make_shared<SliceStrategy>(Orientation::Front_back);
        case VizMode::SliceLeft_right: return std::make_shared<SliceStrategy>(Orientation::Left_right);
        case VizMode::CompositeVolume: return std::make_shared<CompositeStrategy>(VizMode::CompositeVolume);
        case VizMode::CompositeIsoSurface: return std::make_shared<CompositeStrategy>(VizMode::CompositeIsoSurface);
        default: return nullptr;
        }
    }
};
