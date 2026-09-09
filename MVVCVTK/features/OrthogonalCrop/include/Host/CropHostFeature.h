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
    Sphere = 15,
    SaveRoi = 16
};

struct CropHostTarget {
    // 应用选择输入角色和视图；空绑定、空 selector 或空目标集合均拒绝。
    std::string inputBinding;
    HostViewTarget referenceView;
    HostViewTargets targetViews;
};

// Widget actions resume an established target with Root already displayed.
// Use Create/Activate for source/view changes and Select for preview changes.
struct CropHostRequest {
    CropHostAction action = CropHostAction::None;
    std::optional<CropHostTarget> target;
    std::optional<CropRemovalMode> removalMode;
    vtkSmartPointer<vtkPolyData> polyData;
    // BuildResult 可显式采用公共 ROI；此时不消费裁切历史。
    std::optional<DataRevisionRef> inputRoi;
    // SaveRoi 独占字段；只保存当前历史，不生成派生图像/网格。
    std::optional<RoiMetadata> roiMetadata;
    DataBindingRevision expectedCatalogRevision = 0;
};

using CropBuildCallback =
    std::function<void(CropBuildResult)>;

struct CropBuildRequest final {
    CropDocumentId documentId = 0;
    CropNodeId nodeId = 0;
    CropRequestId requestId = 0;
    std::uint64_t expectedRevision = 0;
    CropBuildOptions options;
    // 显式 ROI 从文档 Root 构建，不消费交互历史；结果仍归该文档的独立 scope 所有。
    std::optional<DataRevisionRef> inputRoi;
    bool operator==(const CropBuildRequest& other) const noexcept {
        return documentId==other.documentId && nodeId==other.nodeId && requestId==other.requestId
            && expectedRevision==other.expectedRevision && options==other.options && inputRoi==other.inputRoi;
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

    // SaveRoi 的已接纳请求同步完成 callback；BuildResult 沿用 owner tick 完成。
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
