#pragma once

#include "Host/Types/HostValueTypes.h"

#include <memory>
#include <string>

class FeatureViewLease;
class vtkRenderer;
class vtkRenderWindowInteractor;

struct HostFeatureView final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
};

struct HostInputView final {
    HostFeatureView view;
    vtkRenderer* renderer = nullptr;
    vtkRenderWindowInteractor* interactor = nullptr;
    std::weak_ptr<const FeatureViewLease> lease;
};
