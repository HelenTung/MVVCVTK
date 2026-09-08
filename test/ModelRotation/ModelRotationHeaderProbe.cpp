// 测试用途：编译检查模型旋转公开头可独立包含并完成独立链接。
#include "Host/ModelRotationHostFeature.h"
#include "Host/FeatureModelTransformPort.h"
static_assert(sizeof(ModelRotationState) > 0);
int main()
{
    const auto feature = std::make_shared<ModelRotationHostFeature>();
    return feature->GetState().status == ModelRotationStatus::Detached ? 0 : 1;
}
