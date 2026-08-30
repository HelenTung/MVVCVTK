#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"

#if MVVCVTK_CONSUMER_CROP
#include "Host/HostFeature.h"
#include "Host/CropHostFeature.h"
#endif
#if MVVCVTK_CONSUMER_GAP
#include "Host/HostFeature.h"
#include "Host/GapHostFeature.h"
#endif

#include <memory>
#include <string_view>

#if MVVCVTK_CONSUMER_CROP || MVVCVTK_CONSUMER_GAP
class ConsumerFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override
    {
        return "ConsumerFeature";
    }
    bool AttachHost(const HostFeatureContext&) override { return true; }
    bool DetachHost() override { return true; }
    bool OnHostTick() override { return true; }
};
#endif

int main()
{
    VtkAppHostSession session{ HostSessionConfig{} };
    if (!session.GetIsStopped()) {
        return 1;
    }

    HostViewSetRequest viewRequest;
    HostVolumeTransferFunction volumeTransferFunction;
    volumeTransferFunction.colorNodes = {
        { -1000.0, 0.0, 0.0, 0.0 },
        { 3000.0, 1.0, 1.0, 1.0 }
    };
    volumeTransferFunction.opacityNodes = {
        { -1000.0, 0.0 },
        { 3000.0, 1.0 }
    };
    viewRequest.volumeTransferFunction =
        volumeTransferFunction;
    viewRequest.volumeQuality = HostVolumeQuality::High;
    bool isFeatureContractValid = true;
#if MVVCVTK_CONSUMER_CROP || MVVCVTK_CONSUMER_GAP
    isFeatureContractValid = ConsumerFeature{}.GetFeatureId()
        == std::string_view{ "ConsumerFeature" };
#endif
    if (!viewRequest.volumeTransferFunction
        || !viewRequest.volumeQuality
        || *viewRequest.volumeQuality != HostVolumeQuality::High
        || viewRequest.volumeTransferFunction
            ->colorNodes.size() != 2
        || !isFeatureContractValid) {
        return 4;
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
