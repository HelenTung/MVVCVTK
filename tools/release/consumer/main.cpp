#include "Host/CropHostFeature.h"
#include "Host/GapHostFeature.h"
#include "Host/VtkAppHostSession.h"

#include <memory>
#include <string_view>

int main()
{
    const std::shared_ptr<HostFeature> crop =
        std::make_shared<CropHostFeature>(CropHostConfig{});
    const std::shared_ptr<HostFeature> gap =
        std::make_shared<GapHostFeature>(GapHostConfig{});
    if (!crop || !gap) {
        return 1;
    }
    if (crop->GetFeatureId() !=
            std::string_view{"OrthogonalCrop"}) {
        return 2;
    }
    if (gap->GetFeatureId() !=
            std::string_view{"GapAnalysis"}) {
        return 3;
    }
    return 0;
}
