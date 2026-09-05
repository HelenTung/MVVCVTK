#include "Render/Internal/PartSurfaceProductBuilder.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkExtractVOI.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkUnsignedIntArray.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace {

PartSurfaceBuildResult GetFailure(
    const PartFailureReason reason,
    const std::size_t requiredBytes,
    std::string message)
{
    PartSurfaceBuildResult result;
    result.failureReason = reason;
    result.requiredBytes = requiredBytes;
    result.message = std::move(message);
    return result;
}

bool GetVoxelCount(
    const std::array<int, 3>& dimensions,
    std::size_t& voxelCount) noexcept
{
    voxelCount = 1;
    for (const int dimension : dimensions) {
        if (dimension <= 0
            || static_cast<std::size_t>(dimension)
                > (std::numeric_limits<std::size_t>::max)()
                    / voxelCount) {
            return false;
        }
        voxelCount *= static_cast<std::size_t>(dimension);
    }
    return true;
}

bool GetAddValid(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > (std::numeric_limits<std::size_t>::max)() - left) {
        return false;
    }
    result = left + right;
    return true;
}

struct SurfaceWatch final {
    const std::function<bool()>* getStopRequested = nullptr;
    const PartProgressCallback* sendProgress = nullptr;
    bool hasError = false;
};

} // namespace

PartSurfaceBuildResult PartSurfaceProductBuilder::BuildProduct(
    const PartSurfaceBuildRequest& request,
    const std::function<bool()>& getStopRequested,
    const PartProgressCallback& sendProgress)
{
    if (getStopRequested && getStopRequested()) {
        return GetFailure(
            PartFailureReason::Cancelled, 0,
            "Part surface extraction was cancelled.");
    }
    std::size_t voxelCount = 0;
    if (!request.labels || request.maxWorkingBytes == 0
        || !GetVoxelCount(request.dimensions, voxelCount)
        || request.labels->size() != voxelCount
        || voxelCount > static_cast<std::size_t>(
            (std::numeric_limits<vtkIdType>::max)())) {
        return GetFailure(
            PartFailureReason::InvalidGeometry, 0,
            "Part surface input geometry is invalid.");
    }
    std::size_t labelBytes = 0;
    if (voxelCount
        > (std::numeric_limits<std::size_t>::max)()
            / sizeof(std::uint32_t)) {
        return GetFailure(
            PartFailureReason::BudgetExceeded,
            (std::numeric_limits<std::size_t>::max)(),
            "Part surface label bytes overflow.");
    }
    labelBytes = voxelCount * sizeof(std::uint32_t);
    if (labelBytes > request.maxWorkingBytes) {
        return GetFailure(
            PartFailureReason::BudgetExceeded, labelBytes,
            "Part surface input exceeds the working-set budget.");
    }

    try {
        static_assert(
            std::is_same_v<std::uint32_t, unsigned int>,
            "The Windows x64 Part surface requires uint32_t labels.");
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetExtent(
            request.extent[0], request.extent[1],
            request.extent[2], request.extent[3],
            request.extent[4], request.extent[5]);
        image->SetSpacing(request.spacing.data());
        image->SetOrigin(request.origin.data());
        auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
        direction->DeepCopy(request.direction.data());
        image->SetDirectionMatrix(direction);
        if (static_cast<std::size_t>(image->GetNumberOfPoints())
            != voxelCount) {
            return GetFailure(
                PartFailureReason::InvalidGeometry, labelBytes,
                "Part surface extent does not match its dimensions.");
        }
        auto scalars = vtkSmartPointer<vtkUnsignedIntArray>::New();
        scalars->SetNumberOfComponents(1);
        scalars->SetArray(
            const_cast<unsigned int*>(request.labels->data()),
            static_cast<vtkIdType>(voxelCount),
            1);
        image->GetPointData()->SetScalars(scalars);

        std::size_t inputBytes = labelBytes;
        auto crop = vtkSmartPointer<vtkExtractVOI>::New();
        bool hasCrop = false;
        if (request.foregroundExtent) {
            auto extent = *request.foregroundExtent;
            std::array<int, 3> dimensions{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto low = axis * 2;
                const auto high = low + 1;
                if (extent[low] < request.extent[low] || extent[high] > request.extent[high]
                    || extent[low] > extent[high]) {
                    return GetFailure(PartFailureReason::InvalidGeometry, labelBytes,
                        "Part foreground bounds are outside the source extent.");
                }
                if (extent[low] > request.extent[low]) --extent[low];
                if (extent[high] < request.extent[high]) ++extent[high];
                dimensions[axis] = extent[high] - extent[low] + 1;
            }
            std::size_t cropVoxels = 0;
            if (!GetVoxelCount(dimensions, cropVoxels)) {
                return GetFailure(PartFailureReason::InvalidGeometry, labelBytes,
                    "Part foreground bounds are invalid.");
            }
            // 范围接近全卷时，避免为了微小收益额外复制一个大体积。
            if (cropVoxels <= voxelCount - voxelCount / 4U) {
                if (!GetAddValid(labelBytes, cropVoxels * sizeof(std::uint32_t), inputBytes)
                    || inputBytes > request.maxWorkingBytes) {
                    return GetFailure(PartFailureReason::BudgetExceeded, inputBytes,
                        "Part bounded surface input exceeds the working-set budget.");
                }
                crop->SetInputData(image);
                crop->SetVOI(extent.data());
                hasCrop = true;
            }
        }
        auto contour = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
        if (hasCrop) contour->SetInputConnection(crop->GetOutputPort());
        else contour->SetInputData(image);
        contour->SetNumberOfContours(
            static_cast<int>(request.partCount));
        for (std::uint32_t partId = 1;
            partId <= request.partCount; ++partId) {
            contour->SetValue(
                static_cast<int>(partId - 1),
                static_cast<double>(partId));
        }

        SurfaceWatch watch{
            &getStopRequested, &sendProgress, false
        };
        auto callback = vtkSmartPointer<vtkCallbackCommand>::New();
        callback->SetClientData(&watch);
        callback->SetCallback(
            [](vtkObject* caller,
                const unsigned long eventId,
                void* clientData,
                void*) {
                auto* current = static_cast<SurfaceWatch*>(clientData);
                if (!current) return;
                auto* algorithm = vtkAlgorithm::SafeDownCast(caller);
                if (eventId == vtkCommand::ErrorEvent) {
                    current->hasError = true;
                }
                if (eventId == vtkCommand::ProgressEvent
                    && current->sendProgress
                    && *current->sendProgress && algorithm) {
                    (*current->sendProgress)(
                        algorithm->GetProgress());
                }
                if (current->getStopRequested
                    && *current->getStopRequested
                    && (*current->getStopRequested)()) {
                    if (algorithm) {
                        algorithm->SetAbortExecuteAndUpdateTime();
                    }
                }
            });
        const unsigned long errorTag = contour->AddObserver(
            vtkCommand::ErrorEvent, callback);
        const unsigned long progressTag = contour->AddObserver(
            vtkCommand::ProgressEvent, callback);
        const auto cropErrorTag = crop->AddObserver(vtkCommand::ErrorEvent, callback);
        const auto cropProgressTag = crop->AddObserver(vtkCommand::ProgressEvent, callback);
        contour->Update();
        crop->RemoveObserver(cropErrorTag);
        crop->RemoveObserver(cropProgressTag);
        contour->RemoveObserver(errorTag);
        contour->RemoveObserver(progressTag);
        callback->SetClientData(nullptr);
        if (getStopRequested && getStopRequested()) {
            return GetFailure(
                PartFailureReason::Cancelled, labelBytes,
                "Part surface extraction was cancelled.");
        }
        if (watch.hasError || !contour->GetOutput()) {
            return GetFailure(
                PartFailureReason::InternalError, labelBytes,
                "Part surface extraction reported an error.");
        }

        auto surface = vtkSmartPointer<vtkPolyData>::New();
        surface->ShallowCopy(contour->GetOutput());
        const auto kibibytes = static_cast<std::size_t>(
            surface->GetActualMemorySize());
        constexpr std::size_t bytesPerKib = 1024;
        const std::size_t actualBytes = kibibytes
            <= (std::numeric_limits<std::size_t>::max)() / bytesPerKib
            ? kibibytes * bytesPerKib
            : (std::numeric_limits<std::size_t>::max)();
        std::size_t requiredBytes = 0;
        if (!GetAddValid(inputBytes, actualBytes, requiredBytes)
            || requiredBytes > request.maxWorkingBytes) {
            return GetFailure(
                PartFailureReason::BudgetExceeded,
                requiredBytes,
                "Part surface output exceeds the working-set budget.");
        }
        auto product = std::make_shared<PartSurfaceProduct>();
        product->surface = std::move(surface);
        product->actualBytes = actualBytes;
        PartSurfaceBuildResult result;
        result.product = std::move(product);
        result.requiredBytes = requiredBytes;
        return result;
    }
    catch (const std::bad_alloc&) {
        return GetFailure(
            PartFailureReason::BudgetExceeded,
            request.maxWorkingBytes,
            "Part surface allocation exceeded the working-set budget.");
    }
    catch (...) {
        return GetFailure(
            PartFailureReason::InternalError,
            labelBytes,
            "Part surface extraction raised an internal error.");
    }
}
