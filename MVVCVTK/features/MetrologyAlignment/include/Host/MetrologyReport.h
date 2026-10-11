#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Data/DataPayloads.h"
#include <iosfwd>

struct MetrologyReportRequest final {
    std::string id, title;
    // 每组内不允许重复修订。评价隐含的对齐自动加入，也可显式保留未通过质量门限的对齐。
    std::vector<DataRevisionRef> alignments, inspections, deviations, voxelVolumes;
    // 显式展示配置引用的结果自动加入报告；同一偏差只允许一套色标。
    std::vector<AlignmentDeviationDisplay> deviationDisplays;
    std::optional<AlignmentSceneDisplay> sceneDisplay;
    // 与显式配置互斥。只冻结当前 Feature 配置，不捕获屏幕像素、相机或 VTK 对象。
    bool captureCurrentDisplay=false;
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
enum class MetrologyReportDisplaySource : std::uint8_t { None, ExplicitConfiguration, CurrentFeatureState };
struct MetrologyReportDeviationDisplay final {
    AlignmentDeviationDisplay parameters;
    std::string colorMapVersion="deviation-hue-loop-2";
    // 257 个均匀取值的实际色标 RGB；逐点颜色按同版本分段规则计算，显示截断不修改统计。
    std::array<std::array<std::uint8_t,3>,257> legendRgb{};
    std::array<std::uint8_t,3> invalidRgb{128,128,128},selectedRgb{255,255,0};
    std::optional<AlignmentDeviationSelection> selection;
};
struct MetrologyReportSceneDisplay final {
    std::shared_ptr<const AlignmentSceneDisplay> parameters;
    AlignmentMatrix targetToModel=alignmentIdentity;
    double glyphLength=10;
    std::string glyphVersion="inspection-glyphs-1";
    bool wasCurrentAtCapture=true;
};
struct MetrologyReportPresentation final {
    MetrologyReportDisplaySource source=MetrologyReportDisplaySource::None;
    std::vector<MetrologyReportDeviationDisplay> deviations;
    std::optional<MetrologyReportSceneDisplay> scene;
};
struct MetrologyReport final {
    std::uint32_t schemaVersion=3;
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
    // 原生网格上的单元体积评价，不依赖对齐；共享参数，不持有源体素。
    std::vector<VoxelVolumeSnapshot> voxelVolumes;
    MetrologyReportPresentation presentation;
};
struct MetrologyReportResult final {
    // FullyDetermined 仅表示捕获成功，不代表报告中所有尺寸合格或输入仍为当前版本。
    AlignmentStatus status=AlignmentStatus::Unavailable;
    std::shared_ptr<const MetrologyReport> report;
    std::string message;
};
enum class MetrologyReportFormat : std::uint8_t { Json, Csv };
enum class MetrologyReportWriteStatus : std::uint8_t { Succeeded, InvalidInput, BudgetExceeded, Cancelled, IoError, InternalError, AlreadyExists, ResourceExhausted };
struct MetrologyReportWriteOptions final {
    MetrologyReportFormat format=MetrologyReportFormat::Json;
    bool includeDeviationSamples=false;
    std::function<bool()> isCancelled;
};
struct MetrologyReportWriteResult final {
    MetrologyReportWriteStatus status=MetrologyReportWriteStatus::InvalidInput;
    // 已由 streambuf 确认接收的字节；流抛异常时不能推断异常调用内部已写多少字节。
    std::size_t bytesWritten=0;
    std::string message;
};
// 可在后台只读写出 GetReport 返回的冻结对象；不访问 Host/VTK，不构造整份字符串。
// UTF-8 JSON 保留完整上下文；非有限浮点数用 {"numericState":"NaN|PositiveInfinity|NegativeInfinity"} 表达。
// JSON/CSV schema 3 加入冻结显示配置与色标；CSV configuration_json 列流式保留完整配置。
// 体素行无对齐归属，空矩阵不代表单位矩阵；显示配置行不产生测量值或公差判定。
// CSV 是尺寸/偏差/体素统计视图（偏差行 verdict 属于整个评价）；以空白、公式前缀或单引号起始的文本前加单引号，JSON 保留原文。
// 逐点偏差默认省略并明确标记；CSV 明细的非 Valid 测量值留空。失败可能留下前缀，调用方负责丢弃/原子提交。
MetrologyReportWriteResult WriteMetrologyReport(const MetrologyReport &report,std::ostream &output,
                                               const MetrologyReportWriteOptions &options={});
struct MetrologyReportFileResult final {
    MetrologyReportWriteResult write;
    bool isCommitted=false;
    // 仅清理失败时非空；标识本次创建的临时文件，不授权调用方盲目删除该路径的后来文件。
    std::string remainingTemporaryPath;
};
// Windows x64：UTF-8 常规文件路径，父目录必须存在；不支持设备名/ADS，不覆盖已有目标。
// 在目标目录独占创建临时文件，写出并 FlushFileBuffers 后通过句柄重命名提交；可在后台调用。
// 失败按句柄清理；write.bytesWritten 可能是未提交的临时字节。只有 isCommitted=true 表示目标已提交。
// 提交成功后不因稍后到达的取消而回滚；不承诺掉电时目录项的持久性。
MetrologyReportFileResult WriteMetrologyReportFile(const MetrologyReport &report,const std::string &utf8Path,
                                                  const MetrologyReportWriteOptions &options={});
