#include "IsoSurfaceStrategy.h"
#include "Data/ImageProcessor.h"

#include <vtkAlgorithm.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCallbackCommand.h>
#include <vtkClipPolyData.h>
#include <vtkCommand.h>
#include <vtkCubeAxesActor.h>
#include <vtkDataArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkImageResample.h>
#include <vtkImplicitFunction.h>
#include <vtkMatrix3x3.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkPolyData.h>
#include <vtkPointData.h>
#include <vtkProperty.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
}

class IsoSurfaceStrategy::Mapper final : public vtkOpenGLPolyDataMapper {
public:
    static Mapper* New();
    vtkTypeMacro(Mapper, vtkOpenGLPolyDataMapper);

    void SetEffectBinding(RenderEffectBinding* binding) { m_binding = binding; }
    void RenderPiece(vtkRenderer* renderer, vtkActor* actor) override
    {
        if (m_binding) {
            (void)m_binding->OnRenderStart(renderer);
        }
        this->Superclass::RenderPiece(renderer, actor);
        if (m_binding) {
            (void)m_binding->OnRenderStop();
        }
    }

private:
    RenderEffectBinding* m_binding = nullptr;
};

vtkStandardNewMacro(IsoSurfaceStrategy::Mapper);

class IsoSurfaceStrategy::MaskImplicit final
    : public vtkImplicitFunction {
public:
    static MaskImplicit* New();
    vtkTypeMacro(MaskImplicit, vtkImplicitFunction);

    bool SetMask(vtkImageData* validityMask)
    {
        if (!validityMask
            || validityMask->GetScalarType() != VTK_UNSIGNED_CHAR
            || validityMask->GetNumberOfScalarComponents() != 1) {
            return false;
        }
        m_validityMask = validityMask;
        Modified();
        return true;
    }

    double EvaluateFunction(double point[3]) override
    {
        if (!m_validityMask) {
            return -1.0;
        }
        double continuousIndex[3] = {};
        m_validityMask->TransformPhysicalPointToContinuousIndex(
            point, continuousIndex);
        int extent[6] = {};
        m_validityMask->GetExtent(extent);
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
            m_validityMask->GetScalarPointer(
                index[0], index[1], index[2]));
        return value && *value != 0 ? 1.0 : -1.0;
    }

    void EvaluateGradient(double[3], double gradient[3]) override
    {
        gradient[0] = 0.0;
        gradient[1] = 0.0;
        gradient[2] = 0.0;
    }

private:
    vtkSmartPointer<vtkImageData> m_validityMask;
};

vtkStandardNewMacro(IsoSurfaceStrategy::MaskImplicit);

struct IsoSurfaceStrategy::Pipeline final {
    vtkSmartPointer<vtkImageResample> imageResample;
    vtkSmartPointer<vtkFlyingEdges3D> isoFilter;
    vtkSmartPointer<vtkImageResample> maskResample;
    vtkSmartPointer<vtkImageData> nativeMask;
    vtkSmartPointer<MaskImplicit> maskFunc;
    vtkSmartPointer<vtkClipPolyData> clip;
    std::array<int, 3> inputDimensions{};
    std::array<int, 3> maskDimensions{};

    vtkAlgorithm* GetOutputAlgorithm() const
    {
        return clip
            ? static_cast<vtkAlgorithm*>(clip)
            : static_cast<vtkAlgorithm*>(isoFilter);
    }

    vtkAlgorithmOutput* GetOutputPort() const
    {
        auto* algorithm = GetOutputAlgorithm();
        return algorithm ? algorithm->GetOutputPort() : nullptr;
    }
};

IsoSurfaceStrategy::IsoSurfaceStrategy()
    : m_lodController(std::make_unique<IsoLodController>())
{
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_mapper = vtkSmartPointer<Mapper>::New();

    // predicate 直接读取 vertexMC；禁用 VBO Shift/Scale 才能保持 input-model 坐标。
    m_mapper->SetVBOShiftScaleMethod(
        vtkOpenGLPolyDataMapper::DISABLE_SHIFT_SCALE);
    m_mapper->ScalarVisibilityOff();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationToFlat();
    m_actor->SetPickable(false);
    m_cubeAxes->SetPickable(false);

    AttachProp(m_actor);
    AttachProp(m_cubeAxes);
}

IsoSurfaceStrategy::~IsoSurfaceStrategy() = default;

void IsoSurfaceStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    (void)SetIsoInput(std::move(data), nullptr);
}

bool IsoSurfaceStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    return SetIsoInput(
        std::move(data), std::move(validityMask));
}

void IsoSurfaceStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    (void)SetIsoInput(
        m_lastInput, std::move(validityMask));
}

bool IsoSurfaceStrategy::SetIsoInput(
    vtkSmartPointer<vtkDataObject> data,
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (!data || !m_mapper || !m_lodController) return false;
    if (GetInputCurrent(data, validityMask)) {
        return true;
    }
    if (vtkPolyData::SafeDownCast(data)) {
        return SetPolyInput(std::move(data));
    }

    auto* image = vtkImageData::SafeDownCast(data);
    if (!image) return false;
    const int* dimensions = image->GetDimensions();
    if (!dimensions) return false;

    IsoLodController nextController = *m_lodController;
    IsoLodController::Source source;
    source.dimensions = {
        dimensions[0], dimensions[1], dimensions[2]
    };
    source.nativeBytes = GetImageBytes(image);
    source.maskBytes = GetImageBytes(validityMask);
    source.systemMemoryBytes = GetSystemMemoryBytes();
    source.cpuThreadCount = GetCpuThreadCount();
    if (!nextController.SetSource(source)) return false;

    auto pipeline = BuildCandidate(
        image,
        validityMask,
        nextController.GetProfile().outputDimensions,
        m_currentIsoValue);
    if (!pipeline || !SetPipeline(std::move(pipeline))) {
        return false;
    }

    *m_lodController = std::move(nextController);
    m_lastInput = std::move(data);
    m_lastMask = std::move(validityMask);
    SetInputTimes(m_lastInput, m_lastMask);
    m_cubeAxes->SetBounds(image->GetBounds());
    return true;
}

bool IsoSurfaceStrategy::SetPolyInput(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* poly = vtkPolyData::SafeDownCast(data);
    if (!poly || !m_mapper || !m_lodController) return false;

    // 上游 mesh 没有可降采样的体数据输入；质量意图保留，但不改写几何。
    m_mapper->SetInputData(poly);
    m_mapper->ScalarVisibilityOff();
    m_pipeline.reset();
    (void)m_lodController->Reset();
    m_lastInput = std::move(data);
    m_lastMask = nullptr;
    SetInputTimes(m_lastInput, nullptr);
    m_inputDimensions = {};
    m_maskDimensions = {};
    m_cubeAxes->SetBounds(poly->GetBounds());

    auto* property = m_actor->GetProperty();
    property->SetColor(0.75, 0.75, 0.75);
    property->SetAmbient(0.2);
    property->SetDiffuse(0.8);
    property->SetSpecular(0.15);
    property->SetSpecularPower(15.0);
    property->SetInterpolationToFlat();
    return true;
}

std::unique_ptr<IsoSurfaceStrategy::Pipeline>
IsoSurfaceStrategy::BuildPipeline(
    vtkImageData* image,
    vtkImageData* validityMask,
    const std::array<int, 3>& outputDimensions,
    const double isoValue) const
{
    if (!image || !std::isfinite(isoValue)) return nullptr;
    const int* sourceDimensions = image->GetDimensions();
    if (!sourceDimensions) return nullptr;
    const std::array sourceSize{
        sourceDimensions[0],
        sourceDimensions[1],
        sourceDimensions[2]
    };
    if (std::any_of(
            outputDimensions.begin(),
            outputDimensions.end(),
            [](const int value) { return value <= 0; })) {
        return nullptr;
    }

    auto pipeline = std::make_unique<Pipeline>();
    pipeline->inputDimensions = outputDimensions;
    pipeline->isoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
    pipeline->isoFilter->ComputeNormalsOff();
    pipeline->isoFilter->ComputeGradientsOff();
    pipeline->isoFilter->SetValue(0, isoValue);
    if (outputDimensions == sourceSize) {
        pipeline->isoFilter->SetInputData(image);
    }
    else {
        pipeline->imageResample = ImageProcessor::CreateScaledImage(
            image, outputDimensions);
        if (!pipeline->imageResample) return nullptr;
        pipeline->isoFilter->SetInputConnection(
            pipeline->imageResample->GetOutputPort());
    }

    if (!validityMask) return pipeline;
    if (validityMask->GetScalarType() != VTK_UNSIGNED_CHAR
        || validityMask->GetNumberOfScalarComponents() != 1) {
        return nullptr;
    }
    if (!GetGeometryMatch(image, validityMask)) {
        return nullptr;
    }

    pipeline->maskDimensions = outputDimensions;
    pipeline->maskFunc = vtkSmartPointer<MaskImplicit>::New();
    if (outputDimensions == sourceSize) {
        pipeline->nativeMask = validityMask;
    }
    else {
        pipeline->maskResample = ImageProcessor::CreateScaledMask(
            validityMask, outputDimensions);
        if (!pipeline->maskResample) return nullptr;
    }
    pipeline->clip = vtkSmartPointer<vtkClipPolyData>::New();
    pipeline->clip->SetInputConnection(
        pipeline->isoFilter->GetOutputPort());
    pipeline->clip->SetClipFunction(pipeline->maskFunc);
    pipeline->clip->SetValue(0.0);
    pipeline->clip->InsideOutOff();
    pipeline->clip->GenerateClippedOutputOff();
    return pipeline;
}

std::unique_ptr<IsoSurfaceStrategy::Pipeline>
IsoSurfaceStrategy::BuildCandidate(
    vtkImageData* image,
    vtkImageData* validityMask,
    const std::array<int, 3>& outputDimensions,
    const double isoValue) const
{
    try {
        auto pipeline = BuildPipeline(
            image,
            validityMask,
            outputDimensions,
            isoValue);
        if (!pipeline || !MaterializePipeline(*pipeline)) {
            return nullptr;
        }
        return pipeline;
    }
    catch (const std::exception& error) {
        std::cerr
            << "[IsoLod] candidate pipeline exception: "
            << error.what() << '\n';
    }
    catch (...) {
        std::cerr << "[IsoLod] candidate pipeline unknown exception\n";
    }
    return nullptr;
}

bool IsoSurfaceStrategy::MaterializePipeline(
    Pipeline& pipeline) const
{
    auto* outputAlgorithm = pipeline.GetOutputAlgorithm();
    if (!pipeline.isoFilter || !outputAlgorithm) return false;

    bool hasError = false;
    auto errorCallback = vtkSmartPointer<vtkCallbackCommand>::New();
    errorCallback->SetClientData(&hasError);
    errorCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void*) {
            *static_cast<bool*>(clientData) = true;
        });
    std::array<std::pair<vtkAlgorithm*, unsigned long>, 4> observers{};
    std::size_t observerCount = 0;
    const auto observe = [&](vtkAlgorithm* algorithm) {
        if (!algorithm || observerCount >= observers.size()) return;
        observers[observerCount++] = {
            algorithm,
            algorithm->AddObserver(
                vtkCommand::ErrorEvent, errorCallback)
        };
    };
    observe(pipeline.imageResample);
    observe(pipeline.isoFilter);
    observe(pipeline.maskResample);
    observe(pipeline.clip);

    try {
        vtkImageData* mask = pipeline.nativeMask;
        if (pipeline.maskResample) {
            pipeline.maskResample->Update();
            mask = pipeline.maskResample->GetOutput();
        }
        if (pipeline.maskFunc
            && (!mask || !pipeline.maskFunc->SetMask(mask))) {
            hasError = true;
        }
        if (!hasError) outputAlgorithm->Update();
    }
    catch (const std::exception& error) {
        std::cerr
            << "[IsoLod] candidate materialization exception: "
            << error.what() << '\n';
        hasError = true;
    }
    catch (...) {
        std::cerr
            << "[IsoLod] candidate materialization unknown exception\n";
        hasError = true;
    }

    for (std::size_t index = 0; index < observerCount; ++index) {
        observers[index].first->RemoveObserver(
            observers[index].second);
    }
    return !hasError
        && outputAlgorithm->GetOutputDataObject(0) != nullptr;
}

bool IsoSurfaceStrategy::SetPipeline(
    std::unique_ptr<Pipeline> pipeline)
{
    if (!pipeline || !m_mapper || !pipeline->GetOutputPort()) {
        return false;
    }
    m_mapper->SetInputConnection(pipeline->GetOutputPort());
    m_mapper->ScalarVisibilityOff();
    m_inputDimensions = pipeline->inputDimensions;
    m_maskDimensions = pipeline->maskDimensions;
    m_pipeline = std::move(pipeline);
    return true;
}

bool IsoSurfaceStrategy::GetInputCurrent(
    vtkDataObject* data,
    vtkImageData* validityMask) const
{
    if (!data
        || m_lastInput != data
        || m_lastMask != validityMask
        || m_inputMTime != data->GetMTime()) {
        return false;
    }
    auto* image = vtkImageData::SafeDownCast(data);
    return (!image
            || m_inputScalarMTime == GetScalarTime(image))
        && (!validityMask
            || (m_maskMTime == validityMask->GetMTime()
                && m_maskScalarMTime
                    == GetScalarTime(validityMask)));
}

bool IsoSurfaceStrategy::GetGeometryMatch(
    vtkImageData* image,
    vtkImageData* validityMask) const
{
    if (!image || !validityMask
        || !std::equal(
            image->GetExtent(), image->GetExtent() + 6,
            validityMask->GetExtent())
        || !std::equal(
            image->GetOrigin(), image->GetOrigin() + 3,
            validityMask->GetOrigin(), GetGeometryNear)
        || !std::equal(
            image->GetSpacing(), image->GetSpacing() + 3,
            validityMask->GetSpacing(), GetGeometryNear)) {
        return false;
    }
    auto* imageDirection = image->GetDirectionMatrix();
    auto* maskDirection = validityMask->GetDirectionMatrix();
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

void IsoSurfaceStrategy::SetInputTimes(
    vtkDataObject* data,
    vtkImageData* validityMask)
{
    m_inputMTime = data ? data->GetMTime() : 0;
    m_maskMTime = validityMask ? validityMask->GetMTime() : 0;
    m_inputScalarMTime = GetScalarTime(
        vtkImageData::SafeDownCast(data));
    m_maskScalarMTime = GetScalarTime(validityMask);
}

vtkMTimeType IsoSurfaceStrategy::GetScalarTime(
    vtkImageData* image) const
{
    auto* pointData = image ? image->GetPointData() : nullptr;
    auto* scalars = pointData ? pointData->GetScalars() : nullptr;
    return scalars ? scalars->GetMTime() : 0;
}

std::uint64_t IsoSurfaceStrategy::GetImageBytes(
    vtkImageData* image) const
{
    if (!image) return 0;
    const std::uint64_t kibibytes = static_cast<std::uint64_t>(
        image->GetActualMemorySize());
    constexpr std::uint64_t bytesPerKib = 1024ULL;
    return kibibytes
        <= std::numeric_limits<std::uint64_t>::max() / bytesPerKib
        ? kibibytes * bytesPerKib : 0ULL;
}

std::uint64_t IsoSurfaceStrategy::GetSystemMemoryBytes() const
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return static_cast<std::uint64_t>(status.ullAvailPhys);
    }
#endif
    return 0;
}

unsigned int IsoSurfaceStrategy::GetCpuThreadCount() const noexcept
{
    return std::max(1U, std::thread::hardware_concurrency());
}

void IsoSurfaceStrategy::AttachRenderer(
    vtkSmartPointer<vtkRenderer> renderer)
{
    BaseVisualStrategy::AttachRenderer(renderer);
    m_cubeAxes->SetCamera(renderer->GetActiveCamera());
}

bool IsoSurfaceStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if (!m_actor || !m_lodController) return false;
    const bool hasQuality =
        (flags & UpdateFlags::Quality) != UpdateFlags::None;
    const bool hasIsoValue =
        (flags & UpdateFlags::IsoValue) != UpdateFlags::None;
    if (hasIsoValue && !std::isfinite(params.isoValue)) return false;

    const double nextIsoValue = hasIsoValue
        ? params.isoValue : m_currentIsoValue;
    if (hasQuality) {
        IsoLodController nextController = *m_lodController;
        if (!nextController.SetQuality(params.volumeQuality)) {
            return false;
        }
        auto* image = vtkImageData::SafeDownCast(m_lastInput);
        const bool hasQualityChanged =
            nextController.GetQuality() != m_lodController->GetQuality();
        if (image && hasQualityChanged) {
            auto pipeline = BuildCandidate(
                image,
                m_lastMask,
                nextController.GetProfile().outputDimensions,
                nextIsoValue);
            if (!pipeline || !SetPipeline(std::move(pipeline))) {
                return false;
            }
        }
        *m_lodController = std::move(nextController);
    }

    if (hasIsoValue) {
        m_currentIsoValue = nextIsoValue;
        if (m_pipeline
            && m_pipeline->isoFilter
            && m_pipeline->isoFilter->GetValue(0)
                != m_currentIsoValue) {
            m_pipeline->isoFilter->SetValue(
                0, m_currentIsoValue);
        }
    }

    auto* property = m_actor->GetProperty();
    if ((flags & UpdateFlags::Material) != UpdateFlags::None) {
        property->SetAmbient(params.material.ambient);
        property->SetDiffuse(params.material.diffuse);
        property->SetSpecular(params.material.specular);
        property->SetSpecularPower(params.material.specularPower);
        property->SetOpacity(params.material.opacity);
        if (params.material.isShadeOn) {
            property->SetInterpolationToPhong();
        }
        else {
            property->SetInterpolationToFlat();
        }
    }

    if ((flags & UpdateFlags::Transform) != UpdateFlags::None) {
        Set3DPropsTransform(params.modelMatrix);
    }
    if ((flags & UpdateFlags::Visibility) != UpdateFlags::None) {
        m_cubeAxes->SetVisibility(
            (params.visibilityMask & VisFlags::Ruler) ? 1 : 0);
    }
    return true;
}

vtkProp3D* IsoSurfaceStrategy::GetMainProp()
{
    return m_actor;
}

VolumeQuality IsoSurfaceStrategy::GetQuality() const noexcept
{
    return m_lodController
        ? m_lodController->GetQuality() : VolumeQuality::Auto;
}

std::array<int, 3> IsoSurfaceStrategy::GetLodDimensions(
    const VolumeQuality quality) const noexcept
{
    return m_lodController
        ? m_lodController->GetProfile(quality).outputDimensions
        : std::array<int, 3>{};
}

std::array<int, 3>
IsoSurfaceStrategy::GetInputDimensions() const noexcept
{
    return m_inputDimensions;
}

std::array<int, 3>
IsoSurfaceStrategy::GetMaskDimensions() const noexcept
{
    return m_maskDimensions;
}

RenderEffectTarget IsoSurfaceStrategy::GetRenderEffectTarget() const
{
    RenderEffectTarget target;
    target.targetKind = RenderTargetKind::PolyData;
    target.mapper = m_mapper;
    target.shaderProperty = m_actor
        ? m_actor->GetShaderProperty() : nullptr;
    return target;
}

void IsoSurfaceStrategy::SetEffectBinding(
    RenderEffectBinding* binding)
{
    if (m_mapper) {
        m_mapper->SetEffectBinding(binding);
    }
}
