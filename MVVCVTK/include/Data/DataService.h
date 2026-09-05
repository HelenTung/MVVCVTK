#pragma once

#include "App/AppTypes.h"
#include "Data/ImageReadTypes.h"
#include "Data/VolumeTypes.h"
#include "Host/TrustedDataPort.h"
#include "Platform/TaskStopToken.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

struct DataLoadStage final {
    DataGraphSnapshot baseGraph;
    DataBinding expectedPrimary;
    DataRevisionDraft output;
    DataRevisionRef outputRef;
    VtkImageGridSnapshot image;
};

using DataLoadStageSnapshot = std::shared_ptr<const DataLoadStage>;

class AbstractDataManager : public TrustedDataPort {
public:
    ~AbstractDataManager() noexcept override = default;

    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
    virtual std::optional<ImageDescriptor> GetImageDescriptor() const = 0;
    virtual std::optional<ImageReadState> GetImageReadState() const = 0;
    virtual ImageReadResult GetImageReadResult(
        std::size_t maxReadBytes = imageReadLimit) const = 0;
    virtual ImageReadResult GetImageReadResult(
        const ImageReadRequest& request,
        const TaskStopToken& stopToken) const = 0;
    virtual ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset,
        const TaskStopToken& stopToken) const = 0;
    virtual std::array<double, 2> GetScalarRange() const = 0;
    virtual std::array<double, 3> GetSpacing() const = 0;
    virtual bool SetSpacing(const std::array<double, 3>& spacing) = 0;
    virtual DataBindingRevision GetPrimaryBindingRevision() const = 0;

    virtual bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout) = 0;
    virtual bool SetDataLoaded(
        const std::string& filePath,
        const VolumeLayout& layout,
        const TaskStopToken& stopToken)
    {
        return !stopToken.GetIsStopped()
            && SetDataLoaded(filePath, layout)
            && !stopToken.GetIsStopped();
    }
    virtual bool SetFromBuffer(const VolumeBuffer& buffer) = 0;
    virtual bool SetFromBuffer(
        const VolumeBuffer& buffer,
        const TaskStopToken& stopToken)
    {
        return !stopToken.GetIsStopped()
            && SetFromBuffer(buffer)
            && !stopToken.GetIsStopped();
    }

    virtual DataLoadStageSnapshot GetLoadStage() const = 0;
    virtual bool SetLoadCommit(
        const DataLoadStageSnapshot& expectedStage,
        VtkImageGridSnapshot& published) = 0;
    virtual bool ClearLoadStage() = 0;

    virtual bool ExportData(
        const VtkImageGridSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params) = 0;
    virtual bool ExportData(
        const VtkImageGridSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params,
        const TaskStopToken& stopToken)
    {
        return !stopToken.GetIsStopped()
            && ExportData(imageSnapshot, outputDir, params)
            && !stopToken.GetIsStopped();
    }
    virtual bool ExportSlices(
        const std::string& dirPath,
        Orientation orientation,
        const WindowLevelParams& windowLevel,
        const std::array<double, 16>& modelToWorldMatrix) = 0;
    virtual bool ExportSlices(
        const std::string& dirPath,
        Orientation orientation,
        const WindowLevelParams& windowLevel,
        const std::array<double, 16>& modelToWorldMatrix,
        const TaskStopToken& stopToken)
    {
        return !stopToken.GetIsStopped()
            && ExportSlices(
                dirPath,
                orientation,
                windowLevel,
                modelToWorldMatrix)
            && !stopToken.GetIsStopped();
    }
};
