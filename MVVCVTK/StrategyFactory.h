#pragma once
#include "VisualStrategies.h" 
#include "AppInterfaces.h"
// 工厂引用所有策略

class StrategyFactory {
public:
    static std::shared_ptr<AbstractVisualStrategy> CreateStrategy(VizMode mode) {
        switch (mode) {
        case VizMode::Volume: return std::make_shared<VolumeStrategy>();
        case VizMode::IsoSurface: return std::make_shared<IsoSurfaceStrategy>();
        case VizMode::AxialSlice: return std::make_shared<SliceStrategy>(Orientation::AXIAL);
        case VizMode::CompositeVolume: return std::make_shared<CompositeStrategy>(VizMode::CompositeVolume);
        case VizMode::CompositeIsoSurface: return std::make_shared<CompositeStrategy>(VizMode::CompositeIsoSurface);
        default: return nullptr;
        }
    }
};