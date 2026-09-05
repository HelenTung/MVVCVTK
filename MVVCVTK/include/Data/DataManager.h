#pragma once

#include "Data/DataService.h"

#include <array>
#include <memory>
#include <string>

class BaseDataManager : public AbstractDataManager {
protected:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    bool SetOwnedImage(vtkSmartPointer<vtkImageData> image);
    bool SetLoadImage(
        vtkSmartPointer<vtkImageData> image,
        vtkSmartPointer<vtkImageData> validityMask = {},
        DataProvenance provenance = {});

public:
    BaseDataManager();
    ~BaseDataManager() noexcept override;

    DataGraphSnapshot GetDataGraph() const override;
    DataSnapshot GetData(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override;
    DataQueryResult GetDataQuery(
        const DataGraphSnapshot& graph,
        const DataQuery& query) const override;
    std::optional<DataBinding> GetDataBinding(
        const DataGraphSnapshot& graph,
        std::string_view name) const override;
    ProjectDataSnapshot GetProjectData() const override;
    DataRelationStatus GetDataRelation(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& data,
        std::string_view inputRole,
        std::string_view binding) const override;
    VtkImageGridSnapshot GetImageGrid(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override;
    VtkImageGridSnapshot GetPrimaryImage() const override;
    VtkLabelMapSnapshot GetLabelMap(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override;
    VtkSurfaceMeshSnapshot GetSurfaceMesh(
        const DataGraphSnapshot& graph,
        const DataRevisionRef& ref) const override;
    DataEntityId CreateDataEntityId() override;
    bool SetDataType(DataTypeDescriptor descriptor) override;
    DataCommitResult SetDataCommit(DataTransaction transaction) override;
    DataObserverId AttachDataChange(DataChangeCallback callback) override;
    bool DetachDataChange(DataObserverId observerId) override;

    vtkSmartPointer<vtkImageData> GetVtkImage() const override;
    std::array<double, 2> GetScalarRange() const override;
    std::array<double, 3> GetSpacing() const override;
    bool SetSpacing(const std::array<double, 3>& spacing) override;
    DataBindingRevision GetPrimaryBindingRevision() const override;
    std::optional<ImageReadState> GetImageReadState() const override;
    ImageReadResult GetImageReadResult(
        std::size_t maxReadBytes = imageReadLimit) const override;
    ImageReadResult GetImageReadResult(
        const ImageReadRequest& request,
        const TaskStopToken& stopToken) const override;
    ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset,
        const TaskStopToken& stopToken) const override;

    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    DataLoadStageSnapshot GetLoadStage() const override;
    bool SetLoadCommit(
        const DataLoadStageSnapshot& expectedStage,
        VtkImageGridSnapshot& published) override;
    bool ClearLoadStage() override;

    bool ExportData(
        const VtkImageGridSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params) override;
    bool ExportData(
        const VtkImageGridSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params,
        const TaskStopToken& stopToken) override;
    bool ExportSlices(
        const std::string& dirPath,
        Orientation orientation,
        const WindowLevelParams& windowLevel,
        const std::array<double, 16>& modelToWorldMatrix) override;
    bool ExportSlices(
        const std::string& dirPath,
        Orientation orientation,
        const WindowLevelParams& windowLevel,
        const std::array<double, 16>& modelToWorldMatrix,
        const TaskStopToken& stopToken) override;
};

class RawVolumeDataManager : public BaseDataManager {
public:
    RawVolumeDataManager();
    ~RawVolumeDataManager() noexcept override;
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) override;
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken) override;
    bool SetFromBuffer(const VolumeBuffer& buffer) override;
    bool SetFromBuffer(
        const VolumeBuffer& buffer,
        const TaskStopToken& stopToken) override;
    bool SetImageSnapshot(vtkSmartPointer<vtkImageData> image);
};

class TiffVolumeDataManager : public BaseDataManager {
public:
    TiffVolumeDataManager();
    ~TiffVolumeDataManager() noexcept override;
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) override;
    bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
