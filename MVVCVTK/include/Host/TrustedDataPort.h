#pragma once

#include "Data/DataGraphTypes.h"
#include "Host/RoiReadTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vtkSmartPointer.h>

class vtkImageData;
class vtkPolyData;
class SurfaceMeshPayload;

struct VtkImageGridView final {
    DataGraphSnapshot graph;
    std::optional<DataBinding> binding;
    DataSnapshot data;
    // 可信只读适配：VTK 没有 const 管线输入接口，壳可以共享不可变数组。
    // 写入型 filter 必须分配自己的输出，不能修改借用标量。
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkImageData> validityMask;
};

using VtkImageGridSnapshot =
    std::shared_ptr<const VtkImageGridView>;

struct VtkLabelMapView final {
    DataSnapshot data;
    vtkSmartPointer<vtkImageData> labels;
};

using VtkLabelMapSnapshot =
    std::shared_ptr<const VtkLabelMapView>;

struct VtkSurfaceMeshView final {
    DataSnapshot data;
    vtkSmartPointer<vtkPolyData> mesh;
};

using VtkSurfaceMeshSnapshot =
    std::shared_ptr<const VtkSurfaceMeshView>;

// Render-stage input. Exactly one geometry kind is present; mesh input is never
// represented as an ImageGrid payload. Factories retain the original typed view.
struct VtkRenderInputView final {
    DataGraphSnapshot graph;
    std::optional<DataBinding> binding;
    DataSnapshot data;
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkImageData> validityMask;
    vtkSmartPointer<vtkPolyData> mesh;
    VtkImageGridSnapshot imageView;
    VtkSurfaceMeshSnapshot meshView;
    bool GetValid() const noexcept;
    static std::shared_ptr<const VtkRenderInputView> FromImage(VtkImageGridSnapshot image);
    static std::shared_ptr<const VtkRenderInputView> FromMesh(
        DataGraphSnapshot graph,std::optional<DataBinding> binding,VtkSurfaceMeshSnapshot mesh);
};
using VtkRenderInputSnapshot=std::shared_ptr<const VtkRenderInputView>;

// Worker-prepared trusted views. No graph identity is usable until the matching payload is published.
// Register resourceUse in that output draft; keep the returned cache owner while the result is published.
struct VtkPreparedDataView final {
    static std::shared_ptr<const VtkPreparedDataView> BuildDataView(
        std::shared_ptr<const IDataPayload> payload, VtkImageGridSnapshot source = {});
    static std::shared_ptr<const VtkPreparedDataView> BuildDataView(vtkPolyData* mesh, std::string coordinateFrame = "RAS");
    static std::shared_ptr<const SurfaceMeshPayload> BuildMeshPayload(vtkPolyData* mesh, std::string coordinateFrame = "RAS");
    // Attach a prepublication use to exclusively owned render arrays. Borrowed array memory
    // must be retained by backingOwner, which must not own either VTK container (no cycle).
    // Register the returned use in the output draft before publishing these objects.
    static DataPreparedResource BuildResourceUse(vtkImageData* image, vtkPolyData* mesh,
        std::shared_ptr<const void> backingOwner = {});
    std::shared_ptr<const IDataPayload> payload;
    VtkImageGridSnapshot image;
    VtkSurfaceMeshSnapshot mesh;
    DataPreparedResource resourceUse;
};

struct DataInputSpec final {
    std::string role;
    DataFacetId requiredFacet;
    bool isRequired = true;
};

struct DataOutputSpec final {
    std::string role;
    DataTypeId type;
    std::vector<DataFacetId> facets;
};

struct FeatureDataContract final {
    std::vector<DataInputSpec> inputs;
    std::vector<DataOutputSpec> outputs;
};

class TrustedDataReadPort {
public:
    virtual ~TrustedDataReadPort() noexcept = default;

    virtual RoiReadResult GetRoi(const DataGraphSnapshot& graph,
        const DataRevisionRef& roiRef, const DataRevisionRef& sourceRef) const = 0;
    virtual RoiOperationResult GetRoiOperation(const DataGraphSnapshot&,const RoiOperationRequest&) const = 0;
    virtual RoiReadResult GetRoiPreview(const DataGraphSnapshot&,const RoiDefinition&) const = 0;
    virtual DataGraphSnapshot GetDataGraph() const = 0;
    virtual DataSnapshot GetData(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const = 0;
    virtual DataQueryResult GetDataQuery(
        const DataGraphSnapshot& graph,
        const DataQuery& query) const = 0;
    virtual std::optional<DataBinding> GetDataBinding(
        const DataGraphSnapshot& graph,
        std::string_view name) const = 0;
    virtual ProjectDataSnapshot GetProjectData() const = 0;
    virtual DataLifetimeState GetDataLifetime(const DataEntityId& scopeId) const = 0;
    virtual DataRelationStatus GetDataRelation(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& data,
        std::string_view inputRole,
        std::string_view binding) const = 0;
    virtual VtkImageGridSnapshot GetImageGrid(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const = 0;
    virtual VtkImageGridSnapshot GetPrimaryImage() const = 0;
    virtual VtkLabelMapSnapshot GetLabelMap(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const = 0;
    virtual VtkSurfaceMeshSnapshot GetSurfaceMesh(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const = 0;
};

// 普通同步调用的借用回执；不保存接收器指针，不是异步完成回调。
class DataCommitReceipt {
public:
    virtual ~DataCommitReceipt() noexcept = default;
    // 正式交换且解锁后、通知/可能抛出的后处理前，成功提交同步交付一次。
    // SucceededHistorical 只证输出发布；只有 isActivated/实际 bindings 才证绑定应用。
    // 接收方仅写预分配记录；不得分配、查询、再次请求/提交、取消、Detach/Stop 或派发业务。
    // committed 引用只在本次方法内有效；任意接收方遵守上述约束不能由 noexcept 静态保证。
    virtual void SetDataCommitted(const DataCommitResult& committed) noexcept = 0;
};

class TrustedDataWritePort {
public:
    virtual ~TrustedDataWritePort() noexcept = default;

    virtual std::shared_ptr<const VtkPreparedDataView> SetPreparedDataView(
        const DataRevisionRef&, std::shared_ptr<const VtkPreparedDataView>) { return {}; }
    // 仅 Host owner thread 可写；同步事务不保存异步完成回调。
    virtual RoiResult SetRoi(const RoiRequest& request) = 0;
    virtual RoiOperationResult SetRoiOperation(const RoiOperationRequest&) = 0;
    virtual RoiOperationResult SetRoiOperationCommit(const RoiOperationRequest&,RoiOperationResult) = 0;
    virtual DataEntityId CreateDataEntityId() = 0;
    virtual bool SetDataType(DataTypeDescriptor descriptor) = 0;
    // receipt 仅借用于本次调用，须活到返回/异常展开；Rejected 不交付。
    // 非空时，实际 Succeeded/SucceededHistorical 在所属线程同步交付一次；
    // 不因通知批次或随后异常延迟/重放，也不把它当作显示就绪或最终清理完成。
    // 返回值仍是本次实际结果；回执可早于返回执行，后处理抛出不撤销已交付的提交事实。
    virtual DataCommitResult SetDataCommit(
        DataTransaction transaction, DataCommitReceipt* receipt = nullptr) = 0;
    virtual DataLifetimeState SetDataRelease(const DataEntityId& scopeId) = 0;
    virtual std::unique_ptr<DataChangeBatch> StartDataChanges() = 0;
    virtual DataObserverId AttachDataChange(DataChangeCallback callback) = 0;
    virtual bool DetachDataChange(DataObserverId observerId) = 0;
};

class TrustedDataPort
    : public TrustedDataReadPort
    , public TrustedDataWritePort {
public:
    ~TrustedDataPort() noexcept override = default;
};
