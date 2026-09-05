#include "Render/Internal/IsoSurfaceProductBuilder.h"

#include "Data/ImageProcessor.h"

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

} // namespace

IsoSurfaceBuildResult IsoSurfaceProductBuilder::BuildProduct(
    const IsoSurfaceBuildRequest& request,
    const RenderTaskToken& stopToken) const
{
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

        auto product = std::make_shared<IsoSurfaceProduct>();
        product->requestRevision = request.requestRevision;
        product->inputStamp = request.key.inputStamp;
        product->requestedQuality = request.requestedQuality;
        product->inputDimensions = request.key.outputDimensions;
        product->isoValue = request.key.isoValue;
        product->surface = std::move(surface);
        product->actualBytes = actualBytes;
        product->isPreview = request.isPreview;

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
