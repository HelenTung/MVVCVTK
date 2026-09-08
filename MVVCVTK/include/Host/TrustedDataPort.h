#pragma once

#include "Data/DataGraphTypes.h"

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

class TrustedDataWritePort {
public:
    virtual ~TrustedDataWritePort() noexcept = default;

    virtual std::shared_ptr<const VtkPreparedDataView> SetPreparedDataView(
        const DataRevisionRef&, std::shared_ptr<const VtkPreparedDataView>) { return {}; }
    virtual DataEntityId CreateDataEntityId() = 0;
    virtual bool SetDataType(DataTypeDescriptor descriptor) = 0;
    virtual DataCommitResult SetDataCommit(DataTransaction transaction) = 0;
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
