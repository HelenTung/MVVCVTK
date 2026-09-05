#pragma once

#include "Host/HostFeature.h"
#include "OrthogonalCropTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

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
    SetPrimaryResult,
    SetPolyData,
    ClearPolyData,
    RestoreOriginal,
    Exit
};

struct CropHostTarget {
    HostViewTarget referenceView{
        "", true, HostRenderViewRole::Primary3D };
    HostViewTargets targetViews;
    bool isTargetViewsUsed = false;
    bool isStatusVisible = false;
};

struct CropHostRequest {
    CropHostAction action = CropHostAction::None;
    std::optional<CropHostTarget> target;
    std::optional<CropRemovalMode> removalMode;
    std::optional<std::size_t> nodeCount;
    vtkSmartPointer<vtkPolyData> polyData;
};

struct CropHostKeys {
    HostKeyChord box;
    HostKeyChord plane;
    HostKeyChord noMode;
    HostKeyChord keepMode;
    HostKeyChord removeMode;
    HostKeyChord previous;
    HostKeyChord next;
    HostKeyChord buildResult;
    HostKeyChord restoreOriginal;
    HostKeyChord exit;
    std::array<HostKeyChord, 10> nodes;
};

struct CropHostConfig {
    CropHostTarget defaultTarget;
    HostViewTargets inputViews;
    CropHostKeys keys;
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
    explicit CropHostFeature(CropHostConfig config);
    ~CropHostFeature() noexcept override;

    CropHostFeature(const CropHostFeature&) = delete;
    CropHostFeature& operator=(const CropHostFeature&) = delete;
    CropHostFeature(CropHostFeature&&) = delete;
    CropHostFeature& operator=(CropHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
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
