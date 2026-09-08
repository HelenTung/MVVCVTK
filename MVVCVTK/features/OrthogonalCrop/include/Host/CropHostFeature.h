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
    BuildResult = 8,
    SetPolyData = 10,
    ClearPolyData,
    Exit = 13,
    Cylinder = 14,
    Sphere = 15
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
    vtkSmartPointer<vtkPolyData> polyData;
};

using CropBuildCallback =
    std::function<void(CropBuildResult)>;

struct CropBuildRequest final {
    CropDocumentId documentId = 0;
    CropNodeId nodeId = 0;
    CropRequestId requestId = 0;
    std::uint64_t expectedRevision = 0;
    CropBuildOptions options;
    bool operator==(const CropBuildRequest& other) const noexcept {
        return documentId==other.documentId && nodeId==other.nodeId && requestId==other.requestId
            && expectedRevision==other.expectedRevision && options==other.options;
    }
};
struct CropBuildAdmission final {
    explicit operator bool() const noexcept { return isAccepted; }
    bool isAccepted = false;
    bool isReplay = false;
    CropRequestId requestId = 0;
    CropResultId resultId = 0;
    std::uint64_t stateRevision = 0;
    CropFailure failureReason = CropFailure::None;
};
struct CropBuildOutcome final {
    CropEditStatus status = CropEditStatus::Queued;
    CropBuildResult result;
};

enum class CropDocumentAction : std::uint8_t { ReturnToSource, CloseDocument, CreateDocument, ActivateDocument, RestoreDocument };
struct CropDocumentRequest final {
    CropDocumentAction action=CropDocumentAction::ReturnToSource;
    CropDocumentId documentId=0;
    CropRequestId requestId=0;
    std::uint64_t expectedRevision=0;
    // Create: documentId/expectedRevision are 0 and sourceRevision pins target's
    // binding. Activate: explicit existing document/version; source stays fixed.
    std::optional<CropHostTarget> target;
    std::optional<DataRevisionRef> sourceRevision;
    std::optional<CropDocumentArchive> archive;
    bool restoreResult=true;
    std::size_t availableRamBytes=512ULL*1024*1024;
};
enum class CropRestoreStatus : std::uint8_t { None, HistoryOnly, ResultRestored };
struct CropDocumentOutcome final {
    CropDocumentId documentId=0;
    CropNodeId rootNodeId=0;
    CropRequestId requestId=0;
    std::uint64_t stateRevision=0;
    CropEditStatus status=CropEditStatus::Queued;
    CropDocumentStatus documentStatus=CropDocumentStatus::Ready;
    CropFailure failureReason=CropFailure::None;
    std::vector<DataLifetimeBlocker> blockers;
    CropRestoreStatus restoreStatus=CropRestoreStatus::None;
    std::vector<CropNodeMapping> nodeMappings;
};
struct CropDocumentAdmission final {
    explicit operator bool() const noexcept { return isAccepted; }
    bool isAccepted=false;
    bool isReplay=false;
    CropRequestId requestId=0;
    std::uint64_t stateRevision=0;
    CropFailure failureReason=CropFailure::None;
    CropDocumentId documentId=0;
    CropNodeId rootNodeId=0;
};
using CropEditCallback=std::function<void(CropEditOutcome)>;
using CropDocumentCallback=std::function<void(CropDocumentOutcome)>;

struct CropHostState final {
    CropDocumentStatus documentStatus = CropDocumentStatus::Ready;
    CropFailure failureReason = CropFailure::None;
    std::vector<DataLifetimeBlocker> blockers;
    CropHistoryState history;
    std::vector<CropViewPreviewState> views;
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
    CropBuildAdmission SendRequest(CropBuildRequest request,CropBuildCallback onComplete={});
    std::optional<CropBuildOutcome> GetBuildOutcome(CropDocumentId documentId,CropRequestId requestId) const;
    CropDocumentAdmission SendRequest(CropDocumentRequest request,CropDocumentCallback onComplete={});
    std::optional<CropDocumentOutcome> GetDocumentOutcome(CropDocumentId documentId,CropRequestId requestId) const;
    CropHostState GetState() const;
    CropHostState GetState(CropDocumentId documentId) const;
    // At most 32 live documents; closed documents are not included.
    std::vector<CropDocumentId> GetDocuments() const;
    // Empty while the requested document has an uncommitted edit/build/return.
    std::optional<CropDocumentArchive> GetArchive(CropDocumentId documentId) const;
    // At most 256 input-model sample points; classification uses the last
    // presented predicate and the coordinate-conversion bound from actual input data.
    CropPreviewPrecision GetPreviewPrecision(CropDocumentId documentId,const std::string& viewId,
        const std::vector<CropVectorDouble3Array>& points) const;
    static CropRequestId CreateRequestId() noexcept;
    CropEditAdmission SendRequest(CropEditRequest request,CropEditCallback onComplete={});
    CropHistorySnapshot GetHistory(CropDocumentId documentId = 0,CropNodeId after = 0,std::size_t limit = 1000) const;
    std::optional<CropEditOutcome> GetOutcome(CropDocumentId documentId,CropRequestId requestId) const;
    CropPruneImpact GetPruneImpact(CropDocumentId documentId,const CropPruneRequest& request) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
