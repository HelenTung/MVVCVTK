#include "Host/ModelRotationHostFeature.h"
#include "Host/FeatureModelTransformPort.h"
static_assert(sizeof(ModelRotationState) > 0);
int main()
{
    const auto feature = std::make_shared<ModelRotationHostFeature>();
    return feature->GetState().status == ModelRotationStatus::Detached ? 0 : 1;
}
