#include "../../Data/Support/VtkDataResourceLease.h"
#include "Render/Internal/VolumeLodProductBuilder.h"

#include "Data/ImageProcessor.h"
#include "Render/Internal/RenderWorkBudget.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkImageResample.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkTypeTraits.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr int denoiseIterations = 5;
// 只固定此任务的 native 分块；SMP 的并发 piece 粒度无法在准入前界定 halo。
constexpr int denoiseThreads = 8;
constexpr double denoiseFactor = 0.125;

bool GetGeometryNear(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right)) return false;
    constexpr double geometryTolerance = 1e-12;
    const double scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) <= geometryTolerance * scale;
}

bool GetGeometryMatch(vtkImageData* image, vtkImageData* mask)
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

bool AddBytes(
    const std::uint64_t value,
    std::uint64_t& total) noexcept
{
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
        return false;
    }
    total += value;
    return true;
}

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
            algorithm->AddObserver(vtkCommand::ErrorEvent, m_callback)
        });
        m_observers.push_back({
            algorithm,
            algorithm->AddObserver(vtkCommand::ProgressEvent, m_callback)
        });
    }

    bool GetError() const noexcept { return m_watch.hasError; }

private:
    VtkTaskWatch m_watch;
    vtkSmartPointer<vtkCallbackCommand> m_callback;
    std::vector<std::pair<vtkAlgorithm*, unsigned long>> m_observers;
};

VolumeLodBuildResult GetFailure(
    const RenderProductFailure failure,
    std::string message)
{
    VolumeLodBuildResult result;
    result.failureReason = failure;
    result.message = std::move(message);
    return result;
}

vtkSmartPointer<vtkImageData> GetMaterializedOutput(
    vtkAlgorithm* algorithm)
{
    if (!algorithm) return nullptr;
    auto* output = vtkImageData::SafeDownCast(
        algorithm->GetOutputDataObject(0));
    if (!output || output->GetNumberOfPoints() <= 0
        || !output->GetPointData()
        || !output->GetPointData()->GetScalars()) {
        return nullptr;
    }
    auto result = vtkSmartPointer<vtkImageData>::New();
    result->ShallowCopy(output);
    return result;
}

vtkSmartPointer<vtkImageData> BuildPrivateImage(vtkImageData* input)
{
    try {
        auto* source = input->GetPointData()->GetScalars();
        auto scalars = vtkSmartPointer<vtkDataArray>::Take(source->NewInstance());
        // 标量壳的 information/范围缓存只归候选所有。AOS 数组共享只读 buffer，
        // 同时保留原数组上的借用 owner/资源租约，直到最后一个候选读者释放。
        scalars->ShallowCopy(source);
        auto keepAlive = vtkSmartPointer<vtkCallbackCommand>::New();
        keepAlive->SetClientData(new vtkSmartPointer<vtkDataArray>(source));
        keepAlive->SetClientDataDeleteCallback([](void* value) {
            delete static_cast<vtkSmartPointer<vtkDataArray>*>(value);
        });
        keepAlive->SetCallback([](vtkObject*, unsigned long, void*, void*) {});
        scalars->AddObserver(vtkCommand::DeleteEvent, keepAlive);
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->ShallowCopy(input);
        image->GetPointData()->SetScalars(scalars);
        return image;
    }
    catch (...) {
        // 私有范围缓存只是预热；分配失败仍可沿原生别名/owner 求值路径显示。
        return nullptr;
    }
}

template<class Scalar>
bool SetScalarRanges(const Scalar* values, vtkDataArray* scalars,
    const RenderTaskToken& stopToken)
{
    const auto count = scalars->GetNumberOfTuples();
    const int components = scalars->GetNumberOfComponents();
    const std::array<double, 2> emptyRange{
        static_cast<double>(vtkTypeTraits<Scalar>::Max()),
        static_cast<double>(vtkTypeTraits<Scalar>::Min())};
    std::vector<std::array<double, 2>> ranges(components, emptyRange);
    std::vector<std::array<double, 2>> finiteRanges(components, emptyRange);
    constexpr vtkIdType chunkTuples = 65536;
    for (vtkIdType first = 0; first < count;) {
        if (stopToken.GetIsStopped()) return false;
        const auto last = first + std::min(chunkTuples, count - first);
        for (vtkIdType tuple = first; tuple < last; ++tuple) {
            for (int component = 0; component < components; ++component) {
                const double value = static_cast<double>(values[tuple * components + component]);
                // 比较自然排除 NaN；普通范围保留 Inf，有限范围独立过滤。
                auto& range = ranges[component];
                if (value < range[0]) range[0] = value;
                if (value > range[1]) range[1] = value;
                if (std::isfinite(value)) {
                    auto& finite = finiteRanges[component];
                    if (value < finite[0]) finite[0] = value;
                    if (value > finite[1]) finite[1] = value;
                }
            }
        }
        first = last;
    }
    if (stopToken.GetIsStopped()) return false;
    auto ordinary = vtkSmartPointer<vtkInformationVector>::New();
    auto finite = vtkSmartPointer<vtkInformationVector>::New();
    ordinary->SetNumberOfInformationObjects(components);
    finite->SetNumberOfInformationObjects(components);
    for (int component = 0; component < components; ++component) {
        ordinary->GetInformationObject(component)->Set(
            vtkDataArray::COMPONENT_RANGE(), ranges[component].data(), 2);
        finite->GetInformationObject(component)->Set(
            vtkDataArray::COMPONENT_RANGE(), finiteRanges[component].data(), 2);
    }
    // 整组完成后才填缓存。VTK Modified() 会按原生规则使两组缓存失效。
    scalars->GetInformation()->Set(vtkAbstractArray::PER_COMPONENT(), ordinary);
    scalars->GetInformation()->Set(vtkAbstractArray::PER_FINITE_COMPONENT(), finite);
    return true;
}

bool SetImageRanges(vtkImageData* image, const RenderTaskToken& stopToken)
{
    if (!image) return true;
    auto* scalars = image->GetPointData()->GetScalars();
    const auto* values = scalars->GetVoidPointer(0);
    if (!values) return false;
    bool isReady = false;
    switch (scalars->GetDataType()) {
        vtkTemplateMacro(isReady = SetScalarRanges(static_cast<const VTK_TT*>(values), scalars, stopToken));
    }
    return isReady;
}

} // namespace

std::optional<std::uint64_t> VolumeLodProductBuilder::GetEstimatedBytes(
    const VolumeLodBuildRequest& request)
{
    if (!request.input) return {};
    RenderWorkBudget budget;
    budget.AddGrid(request.input, request.key.outputDimensions);
    budget.AddGrid(request.mask, request.key.outputDimensions);
    budget.Add(1, 4096); // image 数组 KiB 取整及小量管线元数据余量。
    if (request.key.isDenoiseOn) {
        int extent[6]{};
        request.input->GetExtent(extent);
        const int* dimensions = request.input->GetDimensions();
        budget.AddGrid(request.input, {dimensions[0], dimensions[1], dimensions[2]});
        auto splitter = vtkSmartPointer<vtkImageAnisotropicDiffusion3D>::New();
        splitter->SetEnableSMP(false);
        splitter->SetNumberOfThreads(denoiseThreads);
        for (int piece = 0; piece < denoiseThreads; ++piece) {
            int split[6]{};
            const int pieces = splitter->SplitExtent(split, extent, piece, denoiseThreads);
            if (piece >= pieces) break;
            std::uint64_t count = 1;
            for (int axis = 0; axis < 3; ++axis) {
                // 分配的是 InternalRequestUpdateExtent 的完整 halo（KernelMiddle=5），
                // 不是最后一次迭代使用的缩小 extent。
                const auto first = std::max<std::int64_t>(extent[2 * axis],
                    static_cast<std::int64_t>(split[2 * axis]) - denoiseIterations);
                const auto last = std::min<std::int64_t>(extent[2 * axis + 1],
                    static_cast<std::int64_t>(split[2 * axis + 1]) + denoiseIterations);
                const auto size = last - first + 1;
                if (size <= 0 || count > (std::numeric_limits<std::uint64_t>::max)()
                    / static_cast<std::uint64_t>(size)) return {};
                count *= static_cast<std::uint64_t>(size);
            }
            const int components = request.input->GetNumberOfScalarComponents();
            if (components <= 0) return {};
            budget.Add(count, 2ULL * sizeof(double) * components);
        }
    }
    return budget.GetBytes();
}

VolumeLodBuildResult VolumeLodProductBuilder::BuildProduct(
    const VolumeLodBuildRequest& request,
    const RenderTaskToken& stopToken) const
{
    if (!request.inputUse.GetIsPublished()) {
        return GetFailure(RenderProductFailure::StaleInput, "The product source has retired.");
    }
    if (stopToken.GetIsStopped()) {
        return GetFailure(
            RenderProductFailure::Cancelled,
            "The volume LOD build was cancelled.");
    }
    if (!request.input || request.requestRevision == 0
        || std::any_of(
            request.key.outputDimensions.begin(),
            request.key.outputDimensions.end(),
            [](const int value) { return value <= 0; })
        || (request.key.isDenoiseOn
            && (!std::isfinite(request.key.denoiseThreshold)
                || request.key.denoiseThreshold < 0.0))) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The volume LOD build request is invalid.");
    }

    const int* sourceDimensions = request.input->GetDimensions();
    auto* sourcePointData = request.input->GetPointData();
    if (!sourceDimensions || !RenderWorkBudget::GetScalarStorageValid(request.input)
        || !sourcePointData || !sourcePointData->GetScalars()) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The volume LOD source has no scalar volume.");
    }
    const std::array<int, 3> sourceSize{
        sourceDimensions[0], sourceDimensions[1], sourceDimensions[2]
    };
    for (std::size_t axis = 0; axis < sourceSize.size(); ++axis) {
        if (sourceSize[axis] <= 0
            || request.key.outputDimensions[axis] > sourceSize[axis]) {
            return GetFailure(
                RenderProductFailure::InvalidInput,
                "The volume LOD output dimensions are invalid.");
        }
    }
    if (request.mask
        && (!RenderWorkBudget::GetScalarStorageValid(request.mask)
            || request.mask->GetScalarType() != VTK_UNSIGNED_CHAR
            || request.mask->GetNumberOfScalarComponents() != 1
            || !GetGeometryMatch(request.input, request.mask))) {
        return GetFailure(
            RenderProductFailure::InvalidInput,
            "The volume LOD mask geometry is invalid.");
    }

    const auto resources = request.resources.lock();
    if (resources) {
        if (auto cached = resources->GetVolumeProduct(request.key)) {
            if (!stopToken.SetProductOwner(cached, cached->actualBytes))
                return GetFailure(stopToken.GetIsStopped() ? RenderProductFailure::Cancelled
                    : RenderProductFailure::ResourceRejected, "The scalar product lease is unavailable.");
            VolumeLodBuildResult result; result.product = std::move(cached); return result;
        }
    }
    const auto estimate = GetEstimatedBytes(request);
    if ((!estimate && stopToken.GetIsBudgetEnforced())
        || (estimate && !stopToken.SetEstimatedBytes(*estimate))) {
        return GetFailure(RenderProductFailure::ResourceRejected,
            "The volume CPU working set was rejected before allocation.");
    }
    try {
        const bool hasNativeDimensions =
            request.key.outputDimensions == sourceSize;
        vtkSmartPointer<vtkImageData> volume;
        vtkSmartPointer<vtkImageAnisotropicDiffusion3D> denoise;
        vtkSmartPointer<vtkImageResample> volumeResample;
        vtkSmartPointer<vtkImageResample> maskResample;
        VtkObserverSet observers(stopToken);

        if (hasNativeDimensions && !request.key.isDenoiseOn) {
            // 共享只读存储，但不让后台范围计算写入已发布数组的缓存。
            volume = BuildPrivateImage(request.input);
            if (!volume) volume = request.input;
        }
        else {
            if (resources) resources->SetScalarBuildCount();
            vtkAlgorithmOutput* inputPort = nullptr;
            if (request.key.isDenoiseOn) {
                denoise = vtkSmartPointer<
                    vtkImageAnisotropicDiffusion3D>::New();
                denoise->SetEnableSMP(false);
                denoise->SetNumberOfThreads(denoiseThreads);
                denoise->SetInputData(request.input);
                denoise->SetNumberOfIterations(denoiseIterations);
                denoise->SetDiffusionFactor(denoiseFactor);
                denoise->SetDiffusionThreshold(
                    request.key.denoiseThreshold);
                denoise->FacesOn();
                denoise->EdgesOff();
                denoise->CornersOff();
                observers.Add(denoise);
                inputPort = denoise->GetOutputPort();
            }
            volumeResample = ImageProcessor::CreateScaledImage(
                request.input,
                request.key.outputDimensions,
                inputPort);
            if (!volumeResample) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The volume LOD scalar pipeline could not be built.");
            }
            observers.Add(volumeResample);
            volumeResample->Update();
            if (stopToken.GetIsStopped()) {
                return GetFailure(
                    RenderProductFailure::Cancelled,
                    "The volume LOD build was cancelled.");
            }
            if (observers.GetError()) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The volume LOD scalar pipeline reported an error.");
            }
            volume = GetMaterializedOutput(volumeResample);
        }
        if (!volume) {
            return GetFailure(
                RenderProductFailure::BuildFailed,
                "The volume LOD scalar pipeline produced no output.");
        }

        vtkSmartPointer<vtkImageData> mask;
        if (request.mask) {
            if (hasNativeDimensions) {
                mask = BuildPrivateImage(request.mask);
                if (!mask) mask = request.mask;
            }
            else {
                maskResample = ImageProcessor::CreateScaledMask(
                    request.mask,
                    request.key.outputDimensions);
                if (!maskResample) {
                    return GetFailure(
                        RenderProductFailure::BuildFailed,
                        "The volume LOD mask pipeline could not be built.");
                }
                observers.Add(maskResample);
                maskResample->Update();
                if (stopToken.GetIsStopped()) {
                    return GetFailure(
                        RenderProductFailure::Cancelled,
                        "The volume LOD build was cancelled.");
                }
                if (observers.GetError()) {
                    return GetFailure(
                        RenderProductFailure::BuildFailed,
                        "The volume LOD mask pipeline reported an error.");
                }
                mask = GetMaterializedOutput(maskResample);
            }
            if (!mask || !GetGeometryMatch(volume, mask)) {
                return GetFailure(
                    RenderProductFailure::BuildFailed,
                    "The volume LOD mask product has invalid geometry.");
            }
        }

        // PreLoadData 会读取 finite-only 范围。普通范围与有限范围在私有
        // 候选上一次扫描完成，owner 绑定和首次绘制不再触发整卷求值。
        try {
            const bool isVolumeReady = volume == request.input || SetImageRanges(volume, stopToken);
            if (isVolumeReady && mask != request.mask) (void)SetImageRanges(mask, stopToken);
        }
        catch (...) {
            // 缓存预热失败不拒绝产品；VTK 在 owner 的既有路径仍会求出范围。
        }
        if (stopToken.GetIsStopped()) {
            return GetFailure(RenderProductFailure::Cancelled, "The volume LOD build was cancelled.");
        }
        std::uint64_t actualBytes = GetDataBytes(volume);
        if (!AddBytes(GetDataBytes(mask), actualBytes)) {
            return GetFailure(
                RenderProductFailure::ResourceRejected,
                "The volume LOD product size overflowed.");
        }
        if (!stopToken.SetActualBytes(actualBytes)) {
            return GetFailure(
                RenderProductFailure::ResourceRejected,
                "The volume LOD product exceeded its resource lease.");
        }


        if (!request.inputUse.GetIsPublished()) {
            return GetFailure(RenderProductFailure::StaleInput, "The product source retired while building.");
        }
        auto product = std::make_shared<VolumeLodProduct>();
        product->inputUse = request.inputUse;
        product->requestRevision = request.requestRevision;
        product->inputStamp = request.key.inputStamp;
        product->requestedQuality = request.requestedQuality;
        product->outputDimensions = request.key.outputDimensions;
        product->volume = std::move(volume);
        product->mask = std::move(mask);
        product->actualBytes = actualBytes;

        // Native products borrow the already tracked input allocation. Attaching a new result
        // lease to its shared Root scalar would keep that result alive for the entire Root lifetime.
        // New resampled/denoised allocations own the product lease down to their arrays.
        if (!hasNativeDimensions || request.key.isDenoiseOn)
            VtkDataResourceLease::AttachImage(product->volume, product->inputUse.resource);
        if (!hasNativeDimensions)
            VtkDataResourceLease::AttachImage(product->mask, product->inputUse.resource);

        if (!stopToken.SetProductOwner(product, actualBytes)) {
            return GetFailure(RenderProductFailure::ResourceRejected,
                "The product allocation lease was rejected.");
        }
        if (resources) (void)resources->SetVolumeProduct(request.key, product);
        VolumeLodBuildResult result;
        result.product = std::move(product);
        return result;
    }
    catch (...) {
        return GetFailure(
            RenderProductFailure::BuildFailed,
            "The volume LOD build raised an exception.");
    }
}
