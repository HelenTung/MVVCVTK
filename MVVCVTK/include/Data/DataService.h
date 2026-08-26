#pragma once

#include "Data/ImageReadTypes.h"
#include "Data/TrustedImageState.h"
#include "Data/VolumeTypes.h"
#include "Platform/TaskStopToken.h"

#include <array>
#include <optional>
#include <string>

class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;

    virtual TrustedImageSnapshot GetImageSnapshot() const = 0;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
    virtual TrustedImageState GetImageState() const = 0;
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
    virtual bool SetCurrentData(
        TrustedImageState state,
        const TrustedImageSnapshot& expectedSnapshot,
        TrustedImageSnapshot& publishedSnapshot) = 0;
    virtual std::array<double, 2> GetScalarRange() const = 0;
    virtual std::array<double, 3> GetSpacing() const = 0;
    virtual bool SetSpacing(const std::array<double, 3>& spacing) = 0;
    virtual DataVersion GetDataVersion() const = 0;
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
    virtual bool SetCurrentFromPending(bool& hasPending) = 0;
    virtual TrustedImageSnapshot GetPendingSnapshot() const { return {}; }
    // 成功时必须把 expectedPending 的同一个 owner 发布为 current，并返回该 owner。
    virtual bool SetCurrentFromPending(
        const TrustedImageSnapshot& expectedPending,
        TrustedImageSnapshot& publishedSnapshot)
    {
        (void)expectedPending;
        publishedSnapshot.reset();
        return false;
    }
    virtual bool ClearPending() = 0;
    virtual bool ExportData(
        const TrustedImageSnapshot& imageSnapshot,
        const std::string& outputDir,
        const DataExportParams& params) = 0;
    virtual bool ExportData(
        const TrustedImageSnapshot& imageSnapshot,
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
