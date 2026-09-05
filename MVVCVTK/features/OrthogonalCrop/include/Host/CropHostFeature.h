#pragma once

#include "Host/HostFeature.h"
#include "OrthogonalCropTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <vtkSmartPointer.h>

class vtkPolyData;

enum class CropHostAction {
    None,
    Start,
    Box,
    Plane,
    Mode,
    Previous,
    Next,
    Node,
    BuildResult,
    SetPolyData = 10,
    ClearPolyData,
    Exit = 13
};

struct CropHostTarget {
    // 应用选择输入角色和视图；空绑定、空 selector 或空目标集合均拒绝。
    std::string inputBinding;
    HostViewTarget referenceView;
    HostViewTargets targetViews;
};

struct CropHostRequest {
    CropHostAction action = CropHostAction::None;
    std::optional<CropHostTarget> target;
    std::optional<CropRemovalMode> removalMode;
    std::optional<std::size_t> nodeCount;
    vtkSmartPointer<vtkPolyData> polyData;
};

using CropBuildCallback =
    std::function<void(CropBuildResult)>;

struct CropHostState final {
    CropHistoryState history;
    DataCommitId commitId = 0;
    DataRevisionRef sourceRevision;
    DataRevisionRef recipeRevision;
    DataRevisionRef outputRevision;
    bool isActive = false;
    bool isPublishing = false;
};

class CropHostFeature final
    : public HostFeature
    , public std::enable_shared_from_this<CropHostFeature> {
public:
    CropHostFeature();
    ~CropHostFeature() noexcept override;

    CropHostFeature(const CropHostFeature&) = delete;
    CropHostFeature& operator=(const CropHostFeature&) = delete;
    CropHostFeature(CropHostFeature&&) = delete;
    CropHostFeature& operator=(CropHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    std::vector<FeatureOperationState> GetOperationStates() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    bool SendRequest(
        CropHostRequest request,
        CropBuildCallback onComplete = nullptr);
    CropHostState GetState() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
