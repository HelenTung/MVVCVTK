#include "Render/Internal/PartSurfaceProductBuilder.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkExtractVOI.h>
#include <vtkImageData.h>
#include <vtkImageTransform.h>
#include <vtkMatrix3x3.h>
#include <vtkMergePoints.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkFloatArray.h>
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
    double progressBase = 0, progressSpan = 1;
};

struct SurfaceCancelled final {};

std::size_t GetSurfaceBytes(vtkPolyData& surface)
{
    const auto kib = static_cast<std::size_t>(surface.GetActualMemorySize());
    return kib <= std::numeric_limits<std::size_t>::max() / 1024 ? kib * 1024 : std::numeric_limits<std::size_t>::max();
}

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
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto extentSize = static_cast<std::int64_t>(request.extent[2 * axis + 1])
            - request.extent[2 * axis] + 1;
        if (extentSize != request.dimensions[axis])
            return GetFailure(PartFailureReason::InvalidGeometry, labelBytes, "Part surface extent does not match its dimensions.");
    }
    const auto idCount = request.partIds.empty() ? static_cast<std::size_t>(request.partCount) : request.partIds.size();
    const auto sizeLimit = std::numeric_limits<std::size_t>::max();
    std::size_t inputBytes = 0, idBytes = 0, initialBytes = 0;
    if (idCount > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || request.partIds.capacity() > sizeLimit / sizeof(PartLabelId)
        || idCount > sizeLimit / sizeof(PartLabelId)
        || !GetAddValid(labelBytes, request.partIds.capacity() * sizeof(PartLabelId), inputBytes)
        || !GetAddValid(inputBytes, idCount * sizeof(PartLabelId), initialBytes)) {
        return GetFailure(PartFailureReason::BudgetExceeded, sizeLimit, "Part surface IDs overflow.");
    }
    idBytes = idCount * sizeof(PartLabelId);
    if (initialBytes > request.maxWorkingBytes) {
        return GetFailure(
            PartFailureReason::BudgetExceeded, initialBytes,
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
        if (image->GetDataDimension() != 3)
            return GetFailure(PartFailureReason::InternalError, labelBytes, "Part surface extraction needs a three-dimensional label grid.");
        auto scalars = vtkSmartPointer<vtkUnsignedIntArray>::New();
        scalars->SetNumberOfComponents(1);
        scalars->SetArray(
            const_cast<unsigned int*>(request.labels->data()),
            static_cast<vtkIdType>(voxelCount),
            1);
        image->GetPointData()->SetScalars(scalars);

        auto effectiveExtent = request.extent;
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
                effectiveExtent = extent;
            }
        }
        // 按原生体素单元分块，邻块仅共享边界体素；每个单元仍恰好提取一次。
        // 原 vtkDiscreteMarchingCubes 在每层内遍历所有 contour，百万标签时不能及时响应取消。
        constexpr int xyCells = 32, zCells = 8;
        constexpr std::size_t blockPoints = (xyCells + 1) * (xyCells + 1) * (zCells + 1);
        using OrderedLabel = std::pair<PartLabelId, std::size_t>;
        const auto orderBytes = request.partIds.size() * sizeof(OrderedLabel);
        std::size_t fixedBytes = 0;
        if (request.partIds.size() > sizeLimit / sizeof(OrderedLabel) ||
            !GetAddValid(inputBytes, orderBytes, fixedBytes) ||
            !GetAddValid(fixedBytes, blockPoints * (sizeof(PartLabelId) + sizeof(std::uint32_t)),
                         fixedBytes) ||
            !GetAddValid(fixedBytes, idBytes, initialBytes) || initialBytes > request.maxWorkingBytes)
            return GetFailure(PartFailureReason::BudgetExceeded, sizeLimit,
                              "Part block working set exceeds the budget.");
        std::vector<OrderedLabel> order;
        order.reserve(request.partIds.size());
        for (std::size_t i = 0; i < request.partIds.size(); ++i)
        {
            if ((i & 1023U) == 0 && getStopRequested && getStopRequested())
                throw SurfaceCancelled{};
            order.emplace_back(request.partIds[i], i);
        }
        std::size_t sortChecks = 0;
        std::sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
            if ((++sortChecks & 1023U) == 0 && getStopRequested && getStopRequested())
                throw SurfaceCancelled{};
            return a.first < b.first;
        });
        if (std::adjacent_find(order.begin(), order.end(), [](const auto &a, const auto &b) {
                return a.first == b.first;
            }) != order.end())
            return GetFailure(PartFailureReason::InvalidGeometry, initialBytes,
                              "Part display IDs must be unique.");
        const auto findOrder = [&](PartLabelId id) {
            return std::lower_bound(order.begin(), order.end(), id,
                                    [](const auto &item, PartLabelId value) { return item.first < value; });
        };
        // 在原生索引坐标合并共享顶点，再执行与原 VTK 相同的空间变换；大原点不会误焊顶点。
        auto indexImage = vtkSmartPointer<vtkImageData>::New();
        indexImage->SetExtent(request.extent[0], request.extent[1], request.extent[2], request.extent[3],
                              request.extent[4], request.extent[5]);
        indexImage->GetPointData()->SetScalars(scalars);
        auto surface = vtkSmartPointer<vtkPolyData>::New();
        auto points = vtkSmartPointer<vtkPoints>::New();
        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto cellLabels = vtkSmartPointer<vtkFloatArray>::New();
        cellLabels->SetNumberOfComponents(1);
        surface->SetPoints(points);
        surface->SetPolys(polys);
        surface->GetCellData()->SetScalars(cellLabels);
        auto locator = vtkSmartPointer<vtkMergePoints>::New();
        double bounds[6];
        indexImage->GetBounds(bounds);
        locator->InitPointInsertion(points, bounds);
        std::vector<PartLabelId> active;
        active.reserve(blockPoints);
        const auto totalCells = static_cast<std::size_t>(effectiveExtent[1] - effectiveExtent[0]) *
                                static_cast<std::size_t>(effectiveExtent[3] - effectiveExtent[2]) *
                                static_cast<std::size_t>(effectiveExtent[5] - effectiveExtent[4]);
        std::size_t completedCells = 0, requiredBytes = fixedBytes;
        for (std::int64_t z = effectiveExtent[4]; z < effectiveExtent[5]; z += zCells)
            for (std::int64_t y = effectiveExtent[2]; y < effectiveExtent[3]; y += xyCells)
                for (std::int64_t x = effectiveExtent[0]; x < effectiveExtent[1]; x += xyCells)
                {
                    if (getStopRequested && getStopRequested())
                        throw SurfaceCancelled{};
                    const std::array<int, 6> extent{
                        static_cast<int>(x),
                        static_cast<int>(std::min<std::int64_t>(x + xyCells, effectiveExtent[1])),
                        static_cast<int>(y),
                        static_cast<int>(std::min<std::int64_t>(y + xyCells, effectiveExtent[3])),
                        static_cast<int>(z),
                        static_cast<int>(std::min<std::int64_t>(z + zCells, effectiveExtent[5]))};
                    active.clear();
                    for (std::int64_t k = extent[4]; k <= extent[5]; ++k)
                        for (std::int64_t j = extent[2]; j <= extent[3]; ++j)
                        {
                            if (getStopRequested && getStopRequested())
                                throw SurfaceCancelled{};
                            const auto offset =
                                (static_cast<std::size_t>(k - request.extent[4]) * request.dimensions[1] +
                                 static_cast<std::size_t>(j - request.extent[2])) *
                                request.dimensions[0];
                            for (std::int64_t i = extent[0]; i <= extent[1]; ++i)
                            {
                                const auto id =
                                    (*request
                                          .labels)[offset + static_cast<std::size_t>(i - request.extent[0])];
                                const auto selected = order.empty() ? order.end() : findOrder(id);
                                if (id && (order.empty() ? id <= request.partCount
                                                         : selected != order.end() && selected->first == id))
                                    active.push_back(id);
                            }
                        }
                    std::sort(active.begin(), active.end());
                    active.erase(std::unique(active.begin(), active.end()), active.end());
                    if (!order.empty())
                        std::sort(active.begin(), active.end(), [&](auto a, auto b) {
                            return findOrder(a)->second < findOrder(b)->second;
                        });
                    const auto cells = static_cast<std::size_t>(extent[1] - extent[0]) *
                                       (extent[3] - extent[2]) * (extent[5] - extent[4]);
                    if (active.empty())
                    {
                        completedCells += cells;
                        continue;
                    }
                    SurfaceWatch watch{&getStopRequested, &sendProgress, false,
                                       totalCells ? static_cast<double>(completedCells) / totalCells : 0,
                                       totalCells ? static_cast<double>(cells) / totalCells : 0};
                    auto crop = vtkSmartPointer<vtkExtractVOI>::New();
                    crop->SetInputData(indexImage);
                    crop->SetVOI(extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);
                    auto contour = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
                    contour->SetInputConnection(crop->GetOutputPort());
                    contour->SetNumberOfContours(static_cast<int>(active.size()));
                    for (std::size_t i = 0; i < active.size(); ++i)
                        contour->SetValue(static_cast<int>(i), active[i]);
                    auto callback = vtkSmartPointer<vtkCallbackCommand>::New();
                    callback->SetClientData(&watch);
                    callback->SetCallback(
                        [](vtkObject *caller, const unsigned long eventId, void *clientData, void *) {
                            auto *current = static_cast<SurfaceWatch *>(clientData);
                            if (!current)
                                return;
                            auto *algorithm = vtkAlgorithm::SafeDownCast(caller);
                            if (eventId == vtkCommand::ErrorEvent)
                            {
                                current->hasError = true;
                            }
                            if (eventId == vtkCommand::ProgressEvent && current->sendProgress &&
                                *current->sendProgress && algorithm)
                            {
                                (*current->sendProgress)(current->progressBase +
                                                         current->progressSpan * algorithm->GetProgress());
                            }
                            if (current->getStopRequested && *current->getStopRequested &&
                                (*current->getStopRequested)())
                            {
                                if (algorithm)
                                {
                                    algorithm->SetAbortExecuteAndUpdateTime();
                                }
                            }
                        });
                    const unsigned long errorTag = contour->AddObserver(vtkCommand::ErrorEvent, callback);
                    const unsigned long progressTag =
                        contour->AddObserver(vtkCommand::ProgressEvent, callback);
                    const auto cropErrorTag = crop->AddObserver(vtkCommand::ErrorEvent, callback);
                    const auto cropProgressTag = crop->AddObserver(vtkCommand::ProgressEvent, callback);
                    contour->Update();
                    crop->RemoveObserver(cropErrorTag);
                    crop->RemoveObserver(cropProgressTag);
                    contour->RemoveObserver(errorTag);
                    contour->RemoveObserver(progressTag);
                    callback->SetClientData(nullptr);
                    if (getStopRequested && getStopRequested())
                    {
                        return GetFailure(PartFailureReason::Cancelled, labelBytes,
                                          "Part surface extraction was cancelled.");
                    }
                    if (watch.hasError || !contour->GetOutput())
                    {
                        return GetFailure(PartFailureReason::InternalError, labelBytes,
                                          "Part surface extraction reported an error.");
                    }

                    auto *chunk = contour->GetOutput();
                    auto *values = chunk->GetCellData()->GetScalars();
                    if (!GetAddValid(fixedBytes, GetSurfaceBytes(*surface), requiredBytes) ||
                        !GetAddValid(requiredBytes, GetSurfaceBytes(*chunk), requiredBytes) ||
                        requiredBytes > request.maxWorkingBytes)
                        return GetFailure(PartFailureReason::BudgetExceeded, requiredBytes,
                                          "Part block output exceeds the working-set budget.");
                    vtkIdType count = 0;
                    const vtkIdType *ids = nullptr;
                    vtkIdType cell = 0;
                    chunk->GetPolys()->InitTraversal();
                    while (chunk->GetPolys()->GetNextCell(count, ids))
                    {
                        if ((cell & 1023) == 0 && getStopRequested && getStopRequested())
                            throw SurfaceCancelled{};
                        if (count != 3 || !values)
                            return GetFailure(PartFailureReason::InternalError, requiredBytes,
                                              "Part contour output is invalid.");
                        vtkIdType merged[3];
                        for (int i = 0; i < 3; ++i)
                        {
                            double point[3];
                            chunk->GetPoint(ids[i], point);
                            locator->InsertUniquePoint(point, merged[i]);
                        }
                        polys->InsertNextCell(3, merged);
                        cellLabels->InsertNextValue(static_cast<float>(values->GetTuple1(cell++)));
                    }
                    completedCells += cells;
                    if (sendProgress && totalCells)
                        sendProgress(static_cast<double>(completedCells) / totalCells);
                }
        if (getStopRequested && getStopRequested())
            throw SurfaceCancelled{};
        vtkImageTransform::TransformPointSet(image, surface);
        const auto actualBytes = GetSurfaceBytes(*surface);
        std::size_t productBytes = 0;
        if (!GetAddValid(actualBytes, idBytes, productBytes) ||
            !GetAddValid(fixedBytes, productBytes, requiredBytes) || requiredBytes > request.maxWorkingBytes)
        {
            return GetFailure(PartFailureReason::BudgetExceeded, requiredBytes,
                              "Part surface output exceeds the working-set budget.");
        }
        auto product = std::make_shared<PartSurfaceProduct>();
        product->surface = std::move(surface);
        product->partIds.reserve(idCount);
        product->partIds = request.partIds;
        if (product->partIds.empty())
            for (std::uint32_t id = 1; id <= request.partCount; ++id) product->partIds.push_back(id);
        if (product->partIds.capacity() > sizeLimit / sizeof(PartLabelId)
            || !GetAddValid(actualBytes, product->partIds.capacity() * sizeof(PartLabelId), productBytes)
            || !GetAddValid(fixedBytes, productBytes, requiredBytes)
            || requiredBytes > request.maxWorkingBytes)
            return GetFailure(PartFailureReason::BudgetExceeded, sizeLimit, "Part surface retained IDs exceed the budget.");
        product->actualBytes = productBytes;
        PartSurfaceBuildResult result;
        result.product = std::move(product);
        result.requiredBytes = requiredBytes;
        return result;
    }
    catch (const SurfaceCancelled&) {
        return GetFailure(PartFailureReason::Cancelled, labelBytes, "Part surface extraction was cancelled.");
    }
    catch (const std::bad_alloc&) {
        return GetFailure(
            PartFailureReason::BudgetExceeded,
            labelBytes,
            "Part surface allocation exceeded the working-set budget.");
    }
    catch (...) {
        return GetFailure(
            PartFailureReason::InternalError,
            labelBytes,
            "Part surface extraction raised an internal error.");
    }
}
