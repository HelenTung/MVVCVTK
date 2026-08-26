#include "Render/Contracts/RenderStrategyFactory.h"

#include "Render/Strategies/CompositeStrategy.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Strategies/SliceStrategy.h"
#include "Render/Strategies/VolumeStrategy.h"

std::shared_ptr<AbstractVisualStrategy>
CreateRenderStrategy(const VizMode mode)
{
    switch (mode) {
    case VizMode::Volume:
        return std::make_shared<VolumeStrategy>();
    case VizMode::IsoSurface:
        return std::make_shared<IsoSurfaceStrategy>();
    case VizMode::SliceTop_down:
        return std::make_shared<SliceStrategy>(Orientation::Top_down);
    case VizMode::SliceFront_back:
        return std::make_shared<SliceStrategy>(Orientation::Front_back);
    case VizMode::SliceLeft_right:
        return std::make_shared<SliceStrategy>(Orientation::Left_right);
    case VizMode::CompositeVolume:
        return std::make_shared<CompositeStrategy>(
            std::make_shared<VolumeStrategy>());
    case VizMode::CompositeIsoSurface:
        return std::make_shared<CompositeStrategy>(
            std::make_shared<IsoSurfaceStrategy>());
    default:
        return nullptr;
    }
}
