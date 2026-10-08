#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Data/DataPayloads.h"

struct MetrologyReportRequest final {
    std::string id, title;
    // 每组内不允许重复修订。评价隐含的对齐自动加入，也可显式保留未通过质量门限的对齐。
    std::vector<DataRevisionRef> alignments, inspections, deviations;
};
struct MetrologyReportAlignment final {
    DataRevisionRef result, recipeRevision, transform;
    AlignmentInput input;
    AlignmentMatrix sourceToTarget=alignmentIdentity;
    std::string algorithmVersion;
    // 共享已冻结结果，组合报告不复制配准选区、配对点、拟合几何或残差表。
    std::shared_ptr<const AlignmentRecipe> recipe;
    std::shared_ptr<const std::vector<AlignmentGeometry>> geometries;
    std::shared_ptr<const RecordTablePayload> residuals;
    std::shared_ptr<const AlignmentDiagnostics> diagnostics;
    std::shared_ptr<const std::vector<AlignmentMatrix>> initialPoses;
    std::shared_ptr<const AlignmentReferenceSnapshot> reference;
    bool wasCurrentAtCapture=false;
};
struct MetrologyReportImage final {
    GridGeometry3D geometry;
    ImageMetadata metadata;
    ImageValueType valueType=ImageValueType::Unknown;
    std::size_t components=0, valueBytes=0;
    std::array<double,2> scalarRange{};
    bool hasValidityMask=false;
};
struct MetrologyReportMesh final {
    std::size_t vertices=0, triangles=0;
    std::string coordinateFrame;
};
struct MetrologyReportInput final {
    DataRevisionRef revision;
    DataTypeId type;
    std::vector<DataInputRef> inputs;
    std::optional<DataProvenance> provenance;
    // 只复制几何/来源元信息，不持有源体素、三角网格或整个 DataGraph 快照。
    std::optional<MetrologyReportImage> image;
    std::optional<MetrologyReportMesh> mesh;
    bool wasCurrentAtCapture=false;
};
struct MetrologyReport final {
    std::uint32_t schemaVersion=1;
    std::string id, title;
    DataCommitId capturedCommit=0;
    std::int64_t capturedUnixMilliseconds=0;
    std::vector<MetrologyReportAlignment> alignments;
    std::vector<InspectionSnapshot> inspections;
    std::vector<AlignmentDeviationSnapshot> deviations;
    std::vector<MetrologyReportInput> inputs;
    // 所有 current 标志仅描述 capturedCommit，不会随宿主后续修改而变化。
    bool wereInputsCurrentAtCapture=true;
    // 保守的新增元信息预算估计；不含共享结果本体，也不是进程工作集上限。
    std::size_t estimatedMetadataBytes=0;
};
struct MetrologyReportResult final {
    // FullyDetermined 仅表示捕获成功，不代表报告中所有尺寸合格或输入仍为当前版本。
    AlignmentStatus status=AlignmentStatus::Unavailable;
    std::shared_ptr<const MetrologyReport> report;
    std::string message;
};
