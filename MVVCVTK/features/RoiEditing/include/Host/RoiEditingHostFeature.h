#pragma once
#include "Host/HostFeature.h"
#include "Data/RoiTypes.h"
#include <utility>

struct RoiEditingConfig final {
    HostViewTarget referenceView;
    HostViewTargets targetViews;
};
enum class RoiEditingAction : std::uint8_t {
    Begin, SetDraft, Commit, Cancel, SetVisible, ApplySelection,
    BeginOperation, SetOperation, ApplyOperation, CancelOperation, Undo, Redo, ShowRoi, SetMetadata, BeginDrawing, EndDrawing, CancelSave
};
enum class RoiDrawingMode : std::uint8_t { SliceContour, SliceBrush, ProjectedContour, VisibleBrush, Seed, SliceRectangle, SliceEllipse, SliceCircle, EditControlPoint };
struct RoiEditingRequest final {
    RoiEditingAction action = RoiEditingAction::Begin;
    // Begin 的完整草稿；SetDraft 可选携带同源元信息，与有效几何一起更新。
    std::optional<RoiRequest> draft;
    std::optional<RoiPrimitive> primitive;
    std::optional<bool> isVisible;
    std::optional<RoiSelectionMode> selection;
    std::optional<RoiOperationRequest> operation;
    std::optional<DataRevisionRef> roi;
    std::optional<RoiDrawingMode> drawing;
    std::optional<RoiOutputMode> output;
};
enum class RoiDisplayStatus : std::uint8_t { Updating, Displayed, Empty, Failed };
struct RoiEditingState final {
    bool isAttached = false;
    bool hasDraft = false;
    bool isVisible = true;
    bool isDragging = false;
    RoiError error = RoiError::None;
    std::optional<RoiDefinition> draft;
    std::optional<RoiPrimitive> tool;
    std::optional<DataRevisionRef> committedRoi;
    RoiReadSnapshot preview;
    std::uint64_t previewRevision = 0;
    std::size_t undoCount = 0, redoCount = 0;
    bool isComputing = false, isSaving = false;
    RoiError displayError = RoiError::None;
    RoiDisplayStatus displayStatus = RoiDisplayStatus::Empty;
    std::vector<std::pair<DataRevisionRef,RoiError>> displayFailures;
    RoiSelectionMode selection = RoiSelectionMode::Replace;
    RoiOutputMode output = RoiOutputMode::CreateNew;
    RoiOperationResult operation;
    std::vector<DataRevisionRef> visibleRois;
};
// 仓内可选编辑器；请求、VTK 交互和正常 Detach 均在 Host owner thread。
// 草稿不是正式数据，只有 Commit 调用 Host ROI 事务；应用负责组合本 Feature。
class RoiEditingHostFeature final : public HostFeature, public std::enable_shared_from_this<RoiEditingHostFeature> {
public:
    explicit RoiEditingHostFeature(RoiEditingConfig config);
    ~RoiEditingHostFeature() noexcept override;
    RoiEditingHostFeature(const RoiEditingHostFeature&) = delete;
    RoiEditingHostFeature& operator=(const RoiEditingHostFeature&) = delete;
    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;
    RoiResult SendRequest(const RoiEditingRequest& request);
    RoiEditingState GetState() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
