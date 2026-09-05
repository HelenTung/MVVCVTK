#include "Host/HostCoreServices.h"

std::function<std::optional<ImageDescriptor>()> HostCoreServices::GetImageDescriptor() const
{
    const std::weak_ptr<AbstractDataManager> weakData = sharedDataMgr;
    return [weakData]() {
        const auto data = weakData.lock();
        return data ? data->GetImageDescriptor() : std::optional<ImageDescriptor>{};
    };
}

std::function<std::optional<ImageReadState>()>
HostCoreServices::GetImageReadState() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData]() {
        const auto data = weakData.lock();
        return data
            ? data->GetImageReadState()
            : std::optional<ImageReadState>{};
    };
}

std::function<ImageReadResult(const ImageReadRequest&)>
HostCoreServices::GetImageReadResult() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData](const ImageReadRequest& request) {
        const auto data = weakData.lock();
        if (data) {
            return data->GetImageReadResult(
                request, TaskStopToken{});
        }
        return ImageReadResult{};
    };
}

std::function<ImageReadChunkResult(
    const ImageReadRequest&,
    std::size_t)>
HostCoreServices::GetImageReadChunk() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData](
        const ImageReadRequest& request,
        const std::size_t voxelOffset) {
        const auto data = weakData.lock();
        if (data) {
            return data->GetImageReadChunk(
                request, voxelOffset, TaskStopToken{});
        }
        return ImageReadChunkResult{};
    };
}
