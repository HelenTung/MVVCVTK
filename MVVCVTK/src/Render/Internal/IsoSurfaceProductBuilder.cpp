#include "Data/Internal/VtkDataResourceLease.h"
#include "Render/Internal/IsoSurfaceProductBuilder.h"

#include "Data/ImageProcessor.h"
#include "Render/Internal/RenderWorkBudget.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkClipPolyData.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageResample.h>
#include <vtkImplicitFunction.h>
#include <vtkMatrix3x3.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool GetGeometryNear(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right)) return false;
    constexpr double geometryTolerance = 1e-12;
    const double scale = std::max({
        1.0, std::abs(left), std::abs(right)
    });
    return std::abs(left - right) <= geometryTolerance * scale;
}

bool GetGeometryMatch(
    vtkImageData* image,
    vtkImageData* mask)
{
    if (!image || !mask
        || !std::equal(
            image->GetExtent(), image->GetExtent() + 6,
            mask->GetExtent())
        || !std::equal(
            image->GetOrigin(), image->GetOrigin() + 3,
            mask->GetOrigin(), GetGeometryNear)
        || !std::equal(
            image->GetSpacing(), image->GetSpacing() + 3,
            mask->GetSpacing(), GetGeometryNear)) {
        return false;
    }
    auto* imageDirection = image->GetDirectionMatrix();
    auto* maskDirection = mask->GetDirectionMatrix();
    if (!imageDirection || !maskDirection) return false;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (!GetGeometryNear(
                    imageDirection->GetElement(row, column),
                    maskDirection->GetElement(row, column))) {
                return false;
            }
        }
    }
    return true;
}

std::uint64_t GetDataBytes(vtkDataObject* data) noexcept
{
    if (!data) return 0;
    const auto kibibytes = static_cast<std::uint64_t>(
        data->GetActualMemorySize());
    constexpr std::uint64_t bytesPerKib = 1024;
    return kibibytes
        <= (std::numeric_limits<std::uint64_t>::max)() / bytesPerKib
        ? kibibytes * bytesPerKib : 0;
}

class IsoMaskImplicit final : public vtkImplicitFunction {
public:
    static IsoMaskImplicit* New();
    vtkTypeMacro(IsoMaskImplicit, vtkImplicitFunction);

    bool SetMask(vtkImageData* mask)
    {
        if (!mask
            || mask->GetScalarType() != VTK_UNSIGNED_CHAR
            || mask->GetNumberOfScalarComponents() != 1) {
            return false;
        }
        m_mask = mask;
        Modified();
        return true;
    }

    double EvaluateFunction(double point[3]) override
    {
        if (!m_mask) return -1.0;
        double continuousIndex[3] = {};
        m_mask->TransformPhysicalPointToContinuousIndex(
            point, continuousIndex);
        const int* extent = m_mask->GetExtent();
        if (!extent) return -1.0;
        int index[3] = {};
        for (int axis = 0; axis < 3; ++axis) {
            index[axis] = static_cast<int>(
                std::llround(continuousIndex[axis]));
            if (index[axis] < extent[axis * 2]
                || index[axis] > extent[axis * 2 + 1]) {
                return -1.0;
            }
        }
        const auto* value = static_cast<const unsigned char*>(
            m_mask->GetScalarPointer(index[0], index[1], index[2]));
        return value && *value != 0 ? 1.0 : -1.0;
    }

    void EvaluateGradient(double[3], double gradient[3]) override
    {
        gradient[0] = 0.0;
        gradient[1] = 0.0;
        gradient[2] = 0.0;
    }

private:
    vtkSmartPointer<vtkImageData> m_mask;
};

vtkStandardNewMacro(IsoMaskImplicit);

struct VtkTaskWatch final {
    const RenderTaskToken* stopToken = nullptr;
    bool hasError = false;
};

class VtkObserverSet final {
public:
    explicit VtkObserverSet(const RenderTaskToken& stopToken)
    {
        m_watch.stopToken = &stopToken;
        m_callback = vtkSmartPointer<vtkCallbackCommand>::New();
        m_callback->SetClientData(&m_watch);
        m_callback->SetCallback(
            [](vtkObject* caller,
                const unsigned long eventId,
                void* clientData,
                void*) {
                auto* watch = static_cast<VtkTaskWatch*>(clientData);
                if (!watch) return;
                if (eventId == vtkCommand::ErrorEvent) {
                    watch->hasError = true;
                }
                if (watch->stopToken
                    && watch->stopToken->GetIsStopped()) {
                    auto* algorithm = vtkAlgorithm::SafeDownCast(caller);
                    if (algorithm) {
                        algorithm->SetAbortExecuteAndUpdateTime();
                    }
                }
            });
    }

    ~VtkObserverSet()
    {
        for (const auto& observer : m_observers) {
            observer.first->RemoveObserver(observer.second);
        }
        m_callback->SetClientData(nullptr);
    }

    void Add(vtkAlgorithm* algorithm)
    {
        if (!algorithm) return;
        m_observers.push_back({
            algorithm,
            algorithm->AddObserver(
                vtkCommand::ErrorEvent, m_callback)
        });
        m_observers.push_back({
            algorithm,
            algorithm->AddObserver(
                vtkCommand::ProgressEvent, m_callback)
        });
    }

    bool GetError() const noexcept
    {
        return m_watch.hasError;
    }

private:
    VtkTaskWatch m_watch;
    vtkSmartPointer<vtkCallbackCommand> m_callback;
    std::vector<std::pair<vtkAlgorithm*, unsigned long>> m_observers;
};

IsoSurfaceBuildResult GetFailure(
    const RenderProductFailure failure,
    std::string message)
{
    IsoSurfaceBuildResult result;
    result.failureReason = failure;
    result.message = std::move(message);
    return result;
}

template<typename Scalar>
std::optional<std::uint64_t> GetActiveCells(vtkImageData* image,
    const double isoValue, const RenderTaskToken& stopToken)
{
    const auto* values = static_cast<const Scalar*>(image->GetScalarPointer());
    if (!values) return {};
    const int* dims = image->GetDimensions();
    vtkIdType inc[3]{};
    image->GetIncrements(inc);
    const std::array<vtkIdType, 8> corners{0, inc[0], inc[1], inc[1] + inc[0],
        inc[2], inc[2] + inc[0], inc[2] + inc[1], inc[2] + inc[1] + inc[0]};
    std::uint64_t count = 0;
    for (int z = 0; z < dims[2] - 1; ++z) {
        for (int y = 0; y < dims[1] - 1; ++y) {
            for (int x = 0; x < dims[0] - 1; ++x) {
                if (x % 4096 == 0 && stopToken.GetIsStopped()) return {};
                const auto* cell = values + z * inc[2] + y * inc[1] + x * inc[0];
                // 与 VTK 9.4.2 ProcessXEdge 相同：先转 double，再比较 >= iso。
                const bool firstAbove = static_cast<double>(*cell) >= isoValue;
                for (std::size_t corner = 1; corner < corners.size(); ++corner) {
                    if ((static_cast<double>(cell[corners[corner]]) >= isoValue) != firstAbove) {
                        ++count;
                        break;
                    }
                }
            }
        }
    }
    return count;
}

std::optional<std::uint64_t> GetMeshBytes(vtkImageData* image,
    const double isoValue, const bool hasMask, const std::uint64_t imageBytes,
    const RenderTaskToken& stopToken)
{
    std::optional<std::uint64_t> cells;
    switch (image->GetScalarType()) {
        vtkTemplateMacro(cells = GetActiveCells<VTK_TT>(image, isoValue, stopToken));
    default: return {};
    }
    if (!cells || *cells > (std::numeric_limits<std::uint64_t>::max)() / 5) return {};
    const auto triangles = *cells * 5; // 一个 MC cell 至多五个三角面。
    const auto scalarBytes = static_cast<std::uint64_t>(image->GetScalarSize());
    RenderWorkBudget budget;
    budget.Add(1, imageBytes);
    // 不依赖点去重：每三角形三点，float 坐标/scalar + connectivity/offset。
    budget.Add(triangles, 3 * (3 * sizeof(float) + scalarBytes) + 4 * sizeof(vtkIdType));
    budget.Add(1, sizeof(vtkIdType));
    if (hasMask && triangles != 0) {
        // 三角形 clip 最多形成四边形（两个三角形）；同时保留 FE 输出、
        // clip scalar、点定位表和三个预分配 cell array，按不共享新点保守计入。
        budget.Add(triangles, 6 * (3 * sizeof(double) + scalarBytes
            + sizeof(float) + 4 * sizeof(vtkIdType)) + 16 * sizeof(vtkIdType));
        budget.Add(1, 2ULL * 1024 * 1024); // 默认 merge-points 桶和小量容器余量。
    }
    return budget.GetBytes();
}

} // namespace

std::optional<std::uint64_t> IsoSurfaceProductBuilder::GetEstimatedBytes(
    const IsoSurfaceBuildRequest& request)
{
    if (!request.input) return {};
    const auto& dims = request.key.outputDimensions;
    const auto count = RenderWorkBudget::GetVoxelCount(dims);
    if (!count) return {};
    RenderWorkBudget budget;
    budget.AddGrid(request.input, dims);
    budget.AddGrid(request.mask, dims);
    // FlyingEdges 的 XCases 字节数组和每条 X row 的六个 vtkIdType 元数据。
    budget.Add(*count, 1);
    budget.Add(static_cast<std::uint64_t>(dims[1]) * dims[2], 6 * sizeof(vtkIdType));
    budget.Add(1, 4096);
    return budget.GetBytes();
}

IsoSurfaceBuildResult IsoSurfaceProductBuilder::BuildProduct(
    const IsoSurfaceBuildRequest& request,
    const RenderTaskToken& stopToken) const
{
    if (!request.inputUse.GetIsPublished()) {
        return GetFailure(RenderProductFailure::StaleInput, "The product source has retired.");
    }
    if (stopToken.GetIsStopped()) {
        return GetFailure(
            RenderProductFailure::Cancelled,
            "The iso-surface build was cancelled.");
    }
    if (!request.input || request.requestRevision == 0
        || !std::isfinite(request.key.isoValue)
        || std::any_of(
            request.key.outputDimensions.begin(),
            request.key.outputDimensions.end(),
            [](const int value) { return value <= 0; })) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The iso-surface build request is invalid.");
    }

    const int* sourceDimensions = request.input->GetDimensions();
    if (!sourceDimensions
        || request.input->GetNumberOfPoints() <= 0
        || !request.input->GetPointData()
        || !request.input->GetPointData()->GetScalars()) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The iso-surface source has no scalar volume.");
    }
    const std::array<int, 3> sourceSize{
        sourceDimensions[0], sourceDimensions[1], sourceDimensions[2]
    };
    for (std::size_t axis = 0; axis < sourceSize.size(); ++axis) {
        if (sourceSize[axis] <= 0
            || request.key.outputDimensions[axis] > sourceSize[axis]) {
            return GetFailure(
                RenderProductFailure::InvalidInput,
                "The iso-surface output dimensions are invalid.");
        }
    }
    if (request.mask
        && (request.mask->GetScalarType() != VTK_UNSIGNED_CHAR
            || request.mask->GetNumberOfScalarComponents() != 1
            || !GetGeometryMatch(request.input, request.mask))) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The iso-surface mask geometry is invalid.");
    }

    const auto estimate = GetEstimatedBytes(request);
    if (!estimate || !stopToken.SetActualBytes(*estimate)) {
        return GetFailure(RenderProductFailure::ResourceRejected,
            "The iso image working set was rejected before allocation.");
    }
    try {
        auto isoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
        isoFilter->ComputeNormalsOff();
        isoFilter->ComputeGradientsOff();
        isoFilter->SetValue(0, request.key.isoValue);

        vtkSmartPointer<vtkImageResample> imageResample;
        if (request.key.outputDimensions == sourceSize) {
            isoFilter->SetInputData(request.input);
        }
        else {
            imageResample = ImageProcessor::CreateScaledImage(
                request.input,
                request.key.outputDimensions);
            if (!imageResample) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The iso-surface image resample could not be built.");
            }
            isoFilter->SetInputConnection(
                imageResample->GetOutputPort());
        }

        vtkSmartPointer<vtkImageResample> maskResample;
        vtkSmartPointer<IsoMaskImplicit> maskFunction;
        vtkSmartPointer<vtkClipPolyData> clip;
        vtkAlgorithm* outputAlgorithm = isoFilter;
        VtkObserverSet observers(stopToken);
        observers.Add(imageResample);
        observers.Add(isoFilter);
        vtkImageData* builtImage = request.input;
        if (imageResample) {
            imageResample->Update();
            builtImage = imageResample->GetOutput();
        }
        if (stopToken.GetIsStopped()) {
            return GetFailure(RenderProductFailure::Cancelled, "The iso count was cancelled.");
        }
        if (observers.GetError() || !builtImage) {
            return GetFailure(RenderProductFailure::BuildFailed, "The iso image preparation failed.");
        }
        const auto meshBytes = GetMeshBytes(builtImage, request.key.isoValue,
            request.mask != nullptr, *estimate, stopToken);
        if (stopToken.GetIsStopped()) {
            return GetFailure(RenderProductFailure::Cancelled, "The iso count was cancelled.");
        }
        if (!meshBytes || !stopToken.SetActualBytes(*meshBytes)) {
            return GetFailure(RenderProductFailure::ResourceRejected,
                "The iso mesh working set was rejected before extraction.");
        }
        if (request.mask) {
            vtkImageData* builtMask = request.mask;
            if (request.key.outputDimensions != sourceSize) {
                maskResample = ImageProcessor::CreateScaledMask(
                    request.mask,
                    request.key.outputDimensions);
                if (!maskResample) {
                    return GetFailure(
                        RenderProductFailure::BuildFailed,
                        "The iso-surface mask resample could not be built.");
                }
                observers.Add(maskResample);
                maskResample->Update();
                builtMask = maskResample->GetOutput();
            }
            if (stopToken.GetIsStopped()) {
                return GetFailure(
                    RenderProductFailure::Cancelled,
                    "The iso-surface build was cancelled.");
            }
            if (observers.GetError()) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The iso-surface mask resample reported an error.");
            }
            maskFunction = vtkSmartPointer<IsoMaskImplicit>::New();
            if (!maskFunction->SetMask(builtMask)) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The iso-surface mask product is invalid.");
            }
            clip = vtkSmartPointer<vtkClipPolyData>::New();
            clip->SetInputConnection(isoFilter->GetOutputPort());
            clip->SetClipFunction(maskFunction);
            clip->SetValue(0.0);
            clip->InsideOutOff();
            clip->GenerateClippedOutputOff();
            observers.Add(clip);
            outputAlgorithm = clip;
        }
        outputAlgorithm->Update();
        if (stopToken.GetIsStopped()) {
            return GetFailure(
                RenderProductFailure::Cancelled,
                "The iso-surface build was cancelled.");
        }
        if (observers.GetError()) {
            return GetFailure(
                RenderProductFailure::BuildFailed,
                "The iso-surface VTK pipeline reported an error.");
        }

        auto* output = vtkPolyData::SafeDownCast(
            outputAlgorithm->GetOutputDataObject(0));
        if (!output) {
            return GetFailure(
                RenderProductFailure::BuildFailed,
                "The iso-surface VTK pipeline produced no output.");
        }
        auto surface = vtkSmartPointer<vtkPolyData>::New();
        surface->ShallowCopy(output);
        const std::uint64_t actualBytes = GetDataBytes(surface);
        if (!stopToken.SetActualBytes(actualBytes)) {
            return GetFailure(
                RenderProductFailure::ResourceRejected,
                "The iso-surface product exceeded its resource lease.");
        }


        if (!request.inputUse.GetIsPublished()) {
            return GetFailure(RenderProductFailure::StaleInput, "The product source retired while building.");
        }
        auto product = std::make_shared<IsoSurfaceProduct>();
        product->inputUse = request.inputUse;
        product->requestRevision = request.requestRevision;
        product->inputStamp = request.key.inputStamp;
        product->requestedQuality = request.requestedQuality;
        product->inputDimensions = request.key.outputDimensions;
        product->isoValue = request.key.isoValue;
        product->surface = std::move(surface);
        product->actualBytes = actualBytes;
        product->isPreview = request.isPreview;

        VtkDataResourceLease::AttachMesh(product->surface, product->inputUse.resource);

        if (!stopToken.SetProductOwner(product, actualBytes)) {
            return GetFailure(RenderProductFailure::ResourceRejected,
                "The product allocation lease was rejected.");
        }
        IsoSurfaceBuildResult result;
        result.product = std::move(product);
        return result;
    }
    catch (...) {
        return GetFailure(
            RenderProductFailure::BuildFailed,
            "The iso-surface build raised an exception.");
    }
}
