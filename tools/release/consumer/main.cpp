#include "Host/VtkAppHostSession.h"

#if MVVCVTK_CONSUMER_CROP
#include "Host/CropHostFeature.h"
#endif
#if MVVCVTK_CONSUMER_GAP
#include "Host/GapHostFeature.h"
#endif

#include <memory>
#include <string_view>

int main()
{
    VtkAppHostSession session{ HostSessionConfig{} };
    if (!session.GetIsStopped()) {
        return 1;
    }

#if MVVCVTK_CONSUMER_CROP
    const std::shared_ptr<HostFeature> crop =
        std::make_shared<CropHostFeature>(CropHostConfig{});
    if (!crop || crop->GetFeatureId()
            != std::string_view{ "OrthogonalCrop" }) {
        return 2;
    }
#endif

#if MVVCVTK_CONSUMER_GAP
    const std::shared_ptr<HostFeature> gap =
        std::make_shared<GapHostFeature>(GapHostConfig{});
    if (!gap || gap->GetFeatureId()
            != std::string_view{ "GapAnalysis" }) {
        return 3;
    }
#endif

    return 0;
}
