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

    virtual DataEntityId CreateDataEntityId() = 0;
    virtual bool SetDataType(DataTypeDescriptor descriptor) = 0;
    virtual DataCommitResult SetDataCommit(DataTransaction transaction) = 0;
    virtual DataObserverId AttachDataChange(DataChangeCallback callback) = 0;
    virtual bool DetachDataChange(DataObserverId observerId) = 0;
};

class TrustedDataPort
    : public TrustedDataReadPort
    , public TrustedDataWritePort {
public:
    ~TrustedDataPort() noexcept override = default;
};
