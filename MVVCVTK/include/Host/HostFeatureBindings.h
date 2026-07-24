#pragma once

#include "AppInterfaces.h"
#include "Host/HostCoreServices.h"
#include "Host/HostRenderViewSet.h"
#include "Host/Types/HostCommandTypes.h"

#include <memory>

// 主体内建能力只保留 Gap 编排与当前图像快照读取。
// 可选 Feature 通过 HostFeatureContext 挂载，不进入本类。
class HostFeatureBindings final {
public:
    HostFeatureBindings();
    ~HostFeatureBindings();

    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);
    ImageSnapshot GetImageSnapshot() const;
    bool SetImageState(
        ImageState state,
        const ImageSnapshot& expectedSnapshot,
        ImageSnapshot& publishedSnapshot);

    bool StartGap(
        const HostGapStartRequest& request,
        HostCompleteCallback onComplete);
    bool SwitchGapLayer();
    bool ExitGap();
    bool GetGapView() const;
    bool OnHostTick();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
