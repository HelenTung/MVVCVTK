#include "SliceStrategy.h"
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkImageMask.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageProperty.h>
#include <algorithm>
#include <vtkTransform.h>
#include <vtkImageChangeInformation.h>
#include <vtkImageReslice.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageResliceToColors.h>
#include <vtkImageSliceMapper.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLImageSliceMapper.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <cmath>
#include <limits>

namespace {
class EffectSlicePolyMapper final : public vtkOpenGLPolyDataMapper {
public:
    static EffectSlicePolyMapper* New();
    vtkTypeMacro(EffectSlicePolyMapper, vtkOpenGLPolyDataMapper);

    bool SetEffectBinding(RenderEffectBinding* binding)
    {
        m_binding = binding;
        return true;
    }

    bool SetWorldToInput(
        const std::array<double, 16>& worldToInput)
    {
        m_worldToInput = worldToInput;
        return true;
    }

    void RenderPiece(vtkRenderer* renderer, vtkActor* actor) override
    {
        if (m_binding && actor && actor->GetMatrix()) {
            // VTK 在进入内部 PolyData mapper 前才把本帧 SliceToWorld
            // 写入 actor。必须从实际绘制 actor 取矩阵，再转回输入模型空间；
            // 不能依赖外层 mapper 上一时刻缓存的 ResliceMatrix。
            std::array<double, 16> actorToWorld = {};
            std::array<double, 16> localToInput = {};
            vtkMatrix4x4::DeepCopy(actorToWorld.data(), actor->GetMatrix());
            vtkMatrix4x4::Multiply4x4(
                m_worldToInput.data(),
                actorToWorld.data(),
                localToInput.data());
            (void)m_binding->SetLocalToInput(localToInput);
            (void)m_binding->OnRenderStart(renderer);
        }
        this->Superclass::RenderPiece(renderer, actor);
        if (m_binding) {
            (void)m_binding->OnRenderStop();
        }
    }

private:
    RenderEffectBinding* m_binding = nullptr;
    std::array<double, 16> m_worldToInput = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
};

vtkStandardNewMacro(EffectSlicePolyMapper);

class EffectImageSliceMapper final : public vtkOpenGLImageSliceMapper {
public:
    static EffectImageSliceMapper* New();
    vtkTypeMacro(EffectImageSliceMapper, vtkOpenGLImageSliceMapper);

    bool GetEffectTarget(
        vtkMatrix4x4* localToInput,
        const std::array<double, 16>& worldToInput,
        RenderEffectTarget& target)
    {
        if (!localToInput || !this->PolyDataActor) {
            return false;
        }
        auto* mapper = EffectSlicePolyMapper::SafeDownCast(
            this->PolyDataActor->GetMapper());
        if (!mapper) {
            auto* oldMapper = this->PolyDataActor->GetMapper();
            if (!oldMapper) {
                return false;
            }
            auto newMapper = vtkSmartPointer<EffectSlicePolyMapper>::New();
            newMapper->SetInputConnection(oldMapper->GetInputConnection(0, 0));
            this->PolyDataActor->SetMapper(newMapper);
            mapper = newMapper;
        }
        if (!mapper->SetWorldToInput(worldToInput)) {
            return false;
        }
        target.targetKind = RenderTargetKind::Slice;
        target.mapper = mapper;
        target.shaderProperty = this->PolyDataActor->GetShaderProperty();
        vtkMatrix4x4::DeepCopy(target.localToInput.data(), localToInput);
        return true;
    }

    bool SetEffectBinding(
        RenderEffectBinding* binding,
        vtkMatrix4x4* localToInput,
        const std::array<double, 16>& worldToInput)
    {
        RenderEffectTarget target;
        if (!GetEffectTarget(localToInput, worldToInput, target)) {
            return false;
        }
        auto* mapper = EffectSlicePolyMapper::SafeDownCast(target.mapper);
        if (!mapper) {
            return false;
        }
        return mapper->SetEffectBinding(binding);
    }

    bool SetWorldToInput(
        const std::array<double, 16>& worldToInput)
    {
        if (!this->PolyDataActor) {
            return false;
        }
        auto* mapper = EffectSlicePolyMapper::SafeDownCast(
            this->PolyDataActor->GetMapper());
        return !mapper || mapper->SetWorldToInput(worldToInput);
    }
    bool SetResliceRenderState(
        vtkMatrix4x4* dataToWorld,
        const bool isExactPixelMatch,
        const bool isColorDataPassed,
        const bool isMatteEnabled,
        const bool isColorEnabled,
        const bool isDepthEnabled)
    {
        if (!dataToWorld || !this->GetDataToWorldMatrix()) {
            return false;
        }
        this->GetDataToWorldMatrix()->DeepCopy(dataToWorld);
        this->SetExactPixelMatch(isExactPixelMatch);
        this->SetPassColorData(isColorDataPassed);
        this->MatteEnable = isMatteEnabled;
        this->ColorEnable = isColorEnabled;
        this->DepthEnable = isDepthEnabled;
        return true;
    }
};

vtkStandardNewMacro(EffectImageSliceMapper);
}

class SliceStrategy::Mapper final : public vtkImageResliceMapper {
public:
    static Mapper* New();
    vtkTypeMacro(Mapper, vtkImageResliceMapper);

    bool GetEffectTarget(RenderEffectTarget& target)
    {
        auto* sliceMapper = EffectImageSliceMapper::SafeDownCast(
            this->SliceMapper);
        return sliceMapper
            && sliceMapper->GetEffectTarget(
                this->ResliceMatrix,
                m_worldToInput,
                target);
    }

    bool SetEffectBinding(RenderEffectBinding* binding)
    {
        auto* sliceMapper = EffectImageSliceMapper::SafeDownCast(
            this->SliceMapper);
        return sliceMapper
            && sliceMapper->SetEffectBinding(
                binding,
                this->ResliceMatrix,
                m_worldToInput);
    }

    bool SetWorldToInput(
        const std::array<double, 16>& worldToInput)
    {
        if (!std::all_of(
                worldToInput.begin(),
                worldToInput.end(),
                [](const double value) { return std::isfinite(value); })) {
            return false;
        }
        m_worldToInput = worldToInput;
        auto* sliceMapper = EffectImageSliceMapper::SafeDownCast(
            this->SliceMapper);
        return !sliceMapper || sliceMapper->SetWorldToInput(worldToInput);
    }

    bool SetValidityMask(
        vtkImageData* validityMask,
        const double maskedValue)
    {
        if (validityMask
            && (validityMask->GetScalarType() != VTK_UNSIGNED_CHAR
                || validityMask->GetNumberOfScalarComponents() != 1)) {
            return false;
        }
        if (m_validityMask == validityMask
            && m_maskedValue == maskedValue) {
            return true;
        }
        m_validityMask = validityMask;
        m_maskedValue = maskedValue;
        m_isMaskOutputDirty = true;
        Modified();
        return true;
    }

    std::array<int, 3> GetMaskWorkingDimensions() const
    {
        std::array<int, 3> dimensions{};
        auto* output = m_validityMask && m_maskFilter
            ? m_maskFilter->GetOutput() : nullptr;
        if (output) {
            output->GetDimensions(dimensions.data());
        }
        return dimensions;
    }

    bool GetMaskWorkingPixelsValid() const
    {
        auto* sourceMask = m_validityMask.GetPointer();
        auto* sourceSlice = this->ImageReslice
            ? this->ImageReslice->GetOutput() : nullptr;
        auto* maskSlice = m_maskReslice
            ? m_maskReslice->GetOutput() : nullptr;
        auto* maskedSlice = m_maskFilter
            ? m_maskFilter->GetOutput() : nullptr;
        if (!sourceMask || !sourceSlice || !maskSlice || !maskedSlice
            || !sourceMask->GetScalarPointer()
            || !sourceSlice->GetScalarPointer()
            || !maskSlice->GetScalarPointer()
            || !maskedSlice->GetScalarPointer()
            || sourceSlice->GetNumberOfScalarComponents()
                != maskedSlice->GetNumberOfScalarComponents()
            || !std::equal(
                sourceSlice->GetExtent(),
                sourceSlice->GetExtent() + 6,
                maskSlice->GetExtent())
            || !std::equal(
                sourceSlice->GetExtent(),
                sourceSlice->GetExtent() + 6,
                maskedSlice->GetExtent())) {
            return false;
        }

        int dimensions[3] = {};
        maskSlice->GetDimensions(dimensions);
        if (dimensions[0] <= 0 || dimensions[1] <= 0
            || dimensions[2] != 1) {
            return false;
        }

        const int* outputExtent = maskSlice->GetExtent();
        const int* sourceExtent = sourceMask->GetExtent();
        const int componentCount =
            sourceSlice->GetNumberOfScalarComponents();
        bool hasInteriorSourceSample = false;
        for (int z = outputExtent[4]; z <= outputExtent[5]; ++z) {
            for (int y = outputExtent[2]; y <= outputExtent[3]; ++y) {
                for (int x = outputExtent[0]; x <= outputExtent[1]; ++x) {
                    double physicalPoint[3] = {};
                    maskSlice->TransformIndexToPhysicalPoint(
                        x, y, z, physicalPoint);
                    double continuousIndex[3] = {};
                    sourceMask->TransformPhysicalPointToContinuousIndex(
                        physicalPoint, continuousIndex);
                    const int sourceIndex[3] = {
                        static_cast<int>(std::floor(continuousIndex[0] + 0.5)),
                        static_cast<int>(std::floor(continuousIndex[1] + 0.5)),
                        static_cast<int>(std::floor(continuousIndex[2] + 0.5))
                    };
                    const bool isInside =
                        sourceIndex[0] >= sourceExtent[0]
                        && sourceIndex[0] <= sourceExtent[1]
                        && sourceIndex[1] >= sourceExtent[2]
                        && sourceIndex[1] <= sourceExtent[3]
                        && sourceIndex[2] >= sourceExtent[4]
                        && sourceIndex[2] <= sourceExtent[5];
                    const double actualMask =
                        maskSlice->GetScalarComponentAsDouble(x, y, z, 0);
                    // vtkImageReslice 的 border 会在边界外半个 voxel 内延拓；这里只对
                    // 明确落在输入 extent 内的像素核对最近邻来源，边界像素仍在下方核对合成值。
                    if (isInside) {
                        hasInteriorSourceSample = true;
                        const double expectedMask =
                            sourceMask->GetScalarComponentAsDouble(
                                sourceIndex[0], sourceIndex[1], sourceIndex[2], 0);
                        if (actualMask != expectedMask) {
                            return false;
                        }
                    }

                    for (int component = 0;
                        component < componentCount; ++component) {
                        const double sourceValue =
                            sourceSlice->GetScalarComponentAsDouble(
                                x, y, z, component);
                        const double expectedValue = actualMask != 0.0
                            ? sourceValue : m_maskedValue;
                        const double actualValue =
                            maskedSlice->GetScalarComponentAsDouble(
                                x, y, z, component);
                        if (actualValue != expectedValue) {
                            return false;
                        }
                    }
                }
            }
        }
        return hasInteriorSourceSample;
    }

    vtkMTimeType GetMTime() override
    {
        vtkMTimeType modifiedTime = this->Superclass::GetMTime();
        if (m_validityMask) {
            modifiedTime = (std::max)(
                modifiedTime,
                m_validityMask->GetMTime());
        }
        return modifiedTime;
    }

    void Render(
        vtkRenderer* renderer,
        vtkImageSlice* prop) override
    {
        if (!m_validityMask) {
            this->Superclass::Render(renderer, prop);
            return;
        }

        bool isResliceUpdated = false;
        if (this->ResliceNeedUpdate) {
            this->ImageReslice->SetInputConnection(
                this->GetInputConnection(0, 0));
            this->ImageReslice->UpdateWholeExtent();
            this->ResliceNeedUpdate = 0;
            isResliceUpdated = true;
        }

        const vtkMTimeType maskModifiedTime =
            m_validityMask->GetMTime();
        if (isResliceUpdated
            || m_isMaskOutputDirty
            || maskModifiedTime != m_builtMaskModifiedTime) {
            if (!UpdateMaskOutput()) {
                // 暂时性 pipeline 失败不得把 mapper 永久留在空输入；下一帧重新执行主 reslice 与 mask。
                this->ResliceNeedUpdate = 1;
                m_isMaskOutputDirty = true;
                this->SliceMapper->SetInputData(nullptr);
                return;
            }

            // vtkImageResliceMapper 已将当前平面变为二维图像；mask 只在该输出上合成，
            // 不再让三个 Slice 各自物化一份全体积 float vtkImageMask 输出。
            this->ChangeInformation->SetInputData(
                m_maskFilter->GetOutput());
            const double direction[9] = {
                1.0, 0.0, 0.0,
                0.0, 1.0, 0.0,
                0.0, 0.0, 1.0
            };
            this->ChangeInformation->SetOutputDirection(direction);
            double origin[4] = { 0.0, 0.0, 0.0, 1.0 };
            this->ImageReslice->GetOutputOrigin(origin);
            this->DataToSliceMatrix->MultiplyPoint(origin, origin);
            this->ChangeInformation->SetOutputOrigin(origin);
            this->ChangeInformation->UpdateWholeExtent();
            m_builtMaskModifiedTime = maskModifiedTime;
            m_isMaskOutputDirty = false;
        }

        auto* property = prop ? prop->GetProperty() : nullptr;
        if (property
            && property->GetCheckerboard()
            && this->InternalResampleToScreenPixels
            && !this->SeparateWindowLevelOperation
            && this->SliceFacesCamera) {
            this->CheckerboardImage(
                m_maskFilter->GetOutput(),
                renderer->GetActiveCamera(),
                property);
        }

        auto* sliceMapper = EffectImageSliceMapper::SafeDownCast(
            this->SliceMapper);
        if (!sliceMapper
            || !sliceMapper->SetResliceRenderState(
                this->SliceToWorldMatrix,
                this->InternalResampleToScreenPixels != 0,
                !this->SeparateWindowLevelOperation,
                this->MatteEnable,
                this->ColorEnable,
                this->DepthEnable)) {
            return;
        }
        sliceMapper->SetInputData(
            this->ChangeInformation->GetOutput());
        sliceMapper->SetSliceFacesCamera(
            this->SliceFacesCamera
            && !this->SeparateWindowLevelOperation);
        sliceMapper->SetBorder(
            this->Border
            || this->InternalResampleToScreenPixels);
        sliceMapper->SetBackground(
            this->Background
            && !(this->SliceFacesCamera
                && this->InternalResampleToScreenPixels
                && !this->SeparateWindowLevelOperation));
        sliceMapper->SetDisplayExtent(
            this->ImageReslice->GetOutputExtent());
        sliceMapper->SetNumberOfThreads(
            this->NumberOfThreads);
        sliceMapper->SetClippingPlanes(
            this->ClippingPlanes);
        sliceMapper->Render(renderer, prop);
    }

protected:
    Mapper()
    {
        this->SliceMapper->Delete();
        this->SliceMapper = EffectImageSliceMapper::New();
        m_maskReslice = vtkSmartPointer<vtkImageReslice>::New();
        m_maskReslice->SetInterpolationModeToNearestNeighbor();
        m_maskReslice->SetOutputScalarType(VTK_UNSIGNED_CHAR);
        m_maskReslice->SetBackgroundLevel(0.0);
        m_maskFilter = vtkSmartPointer<vtkImageMask>::New();
        m_maskFilter->NotMaskOff();
    }

private:
    bool UpdateMaskOutput()
    {
        auto* sliceImage = this->ImageReslice->GetOutput();
        if (!m_validityMask
            || !sliceImage
            || !sliceImage->GetScalarPointer()
            || !m_maskReslice
            || !m_maskFilter) {
            return false;
        }

        m_maskReslice->SetInputData(m_validityMask);
        // vtkImageResliceMapper 通过输出的物理 origin/direction/spacing 描述当前平面；
        // 同时复制可选 axes/transform，确保未来自定义 reslice 状态也与主图完全一致。
        m_maskReslice->SetResliceAxes(
            this->ImageReslice->GetResliceAxes());
        m_maskReslice->SetResliceTransform(
            this->ImageReslice->GetResliceTransform());
        m_maskReslice->SetOutputExtent(
            this->ImageReslice->GetOutputExtent());
        m_maskReslice->SetOutputSpacing(
            this->ImageReslice->GetOutputSpacing());
        m_maskReslice->SetOutputOrigin(
            this->ImageReslice->GetOutputOrigin());
        m_maskReslice->SetOutputDirection(
            this->ImageReslice->GetOutputDirection());
        m_maskReslice->SetBorder(this->ImageReslice->GetBorder());
        m_maskReslice->SetBorderThickness(
            this->ImageReslice->GetBorderThickness());
        m_maskReslice->SetWrap(this->ImageReslice->GetWrap());
        m_maskReslice->SetMirror(this->ImageReslice->GetMirror());
        m_maskReslice->UpdateWholeExtent();
        auto* maskSlice = m_maskReslice->GetOutput();
        if (!maskSlice
            || !maskSlice->GetScalarPointer()
            || !std::equal(
                sliceImage->GetExtent(),
                sliceImage->GetExtent() + 6,
                maskSlice->GetExtent())) {
            return false;
        }

        m_maskFilter->SetImageInputData(sliceImage);
        m_maskFilter->SetMaskInputData(maskSlice);
        m_maskFilter->SetMaskedOutputValue(m_maskedValue);
        m_maskFilter->UpdateWholeExtent();
        auto* maskedSlice = m_maskFilter->GetOutput();
        return maskedSlice
            && maskedSlice->GetScalarPointer()
            && std::equal(
                sliceImage->GetExtent(),
                sliceImage->GetExtent() + 6,
                maskedSlice->GetExtent());
    }

    std::array<double, 16> m_worldToInput = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    vtkSmartPointer<vtkImageData> m_validityMask;
    vtkSmartPointer<vtkImageReslice> m_maskReslice;
    vtkSmartPointer<vtkImageMask> m_maskFilter;
    vtkMTimeType m_builtMaskModifiedTime = 0;
    bool m_isMaskOutputDirty = true;
    double m_maskedValue = 0.0;
};

vtkStandardNewMacro(SliceStrategy::Mapper);

void SliceStrategy::SetWorldBounds(const double bounds[6],
    const std::array<double, 16>& modelMatrix,
    double worldBounds[6]) const
{
    // 把局部轴对齐包围盒的 8 个顶点全部变换到世界空间，
    // 重新求 min/max，避免模型旋转后仍沿用旧的局部 bounds。
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorldMatrix->DeepCopy(modelMatrix.data());

    worldBounds[0] = worldBounds[2] = worldBounds[4] = std::numeric_limits<double>::max();
    worldBounds[1] = worldBounds[3] = worldBounds[5] = std::numeric_limits<double>::lowest();

    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                double modelToWorldInputPoint[4] = {
                    ix == 0 ? bounds[0] : bounds[1],
                    iy == 0 ? bounds[2] : bounds[3],
                    iz == 0 ? bounds[4] : bounds[5],
                    1.0
                };
                double modelToWorldOutputPoint[4] = { 0.0, 0.0, 0.0, 1.0 };
                modelToWorldMatrix->MultiplyPoint(modelToWorldInputPoint, modelToWorldOutputPoint);

                const double invW = std::abs(modelToWorldOutputPoint[3]) > 1e-12 ? 1.0 / modelToWorldOutputPoint[3] : 1.0;
                const double x = modelToWorldOutputPoint[0] * invW;
                const double y = modelToWorldOutputPoint[1] * invW;
                const double z = modelToWorldOutputPoint[2] * invW;

                worldBounds[0] = std::min(worldBounds[0], x);
                worldBounds[1] = std::max(worldBounds[1], x);
                worldBounds[2] = std::min(worldBounds[2], y);
                worldBounds[3] = std::max(worldBounds[3], y);
                worldBounds[4] = std::min(worldBounds[4], z);
                worldBounds[5] = std::max(worldBounds[5], z);
            }
        }
    }
}

SliceStrategy::SliceStrategy(Orientation orient) : m_orientation(orient) {
    m_slice = vtkSmartPointer<vtkImageSlice>::New();
    m_mapper = vtkSmartPointer<Mapper>::New();
    m_slicePlane = vtkSmartPointer<vtkPlane>::New();
    m_mapper->SetSlicePlane(m_slicePlane);

    // 十字线与切片主图像分开建模，这样 Cursor 更新只需改 line source，
    // 不会触发切片图像自身的 reslice 管线重建。
    m_vLineSource = vtkSmartPointer<vtkLineSource>::New();
    m_hLineSource = vtkSmartPointer<vtkLineSource>::New();

    m_vLineActor = vtkSmartPointer<vtkActor>::New();
    m_hLineActor = vtkSmartPointer<vtkActor>::New();

    // 设置 Mapper
    auto vMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    vMapper->SetInputConnection(m_vLineSource->GetOutputPort());
    m_vLineActor->SetMapper(vMapper);

    auto hMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    hMapper->SetInputConnection(m_hLineSource->GetOutputPort());
    m_hLineActor->SetMapper(hMapper);

    m_vLineActor->GetProperty()->SetLineWidth(1.5);
    // 十字线使用纯色且关闭光照；SetCrosshair 会沿切片法线偏移一个最小轴 spacing，避免共面闪烁。
    m_vLineActor->GetProperty()->SetLighting(false); // 关闭光照，纯色显示

    m_hLineActor->GetProperty()->SetLineWidth(1.5);
    m_hLineActor->GetProperty()->SetLighting(false);

    // 不使用 LUT 自身的 scalar range，显示范围由 vtkImageProperty 的 Window/Level 控制。
    m_slice->GetProperty()->SetUseLookupTableScalarRange(0);
    m_mapper->SliceFacesCameraOff(); // 截面永远绝对平行于相机屏幕
    m_mapper->SliceAtFocalPointOff(); // 截面不强制穿过相机焦点，由状态统一驱动

    if (m_orientation == Orientation::Top_down) {
        m_slicePlane->SetNormal(0, 0, 1); // Z轴法线
        m_vLineActor->GetProperty()->SetColor(1, 0, 0);
        m_hLineActor->GetProperty()->SetColor(0, 1, 0);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
    }
    else if (m_orientation == Orientation::Front_back) {
        m_slicePlane->SetNormal(0, 1, 0); // Y轴法线
        m_vLineActor->GetProperty()->SetColor(1, 0, 0);
        m_hLineActor->GetProperty()->SetColor(0, 0, 1);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
    }
    else {
        m_slicePlane->SetNormal(1, 0, 0); // X轴法线
        m_vLineActor->GetProperty()->SetColor(0, 1, 0);
        m_hLineActor->GetProperty()->SetColor(0, 0, 1);
        m_vLineActor->GetProperty()->SetOpacity(0.4);
        m_hLineActor->GetProperty()->SetOpacity(0.4);
    }

    AttachProp(m_slice);
    AttachProp(m_vLineActor);
    AttachProp(m_hLineActor);
}

SliceStrategy::~SliceStrategy() = default;

void SliceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    const vtkMTimeType geometryMTime = img->GetMTime();
    if (m_lastInput == data && m_mapper->GetInput()
        && m_inputGeometryMTime == geometryMTime) {
        return;
    }
    const bool isSameInput = m_lastInput == data
        && m_mapper->GetInput();
    m_lastInput = data;
    m_inputGeometryMTime = geometryMTime;
    std::copy_n(
        img->GetBounds(), m_inputBounds.size(),
        m_inputBounds.begin());
    std::copy_n(
        img->GetSpacing(), m_inputSpacing.size(),
        m_inputSpacing.begin());
    m_hasTransformCache = false;
    if (!isSameInput) {
        (void)m_mapper->SetValidityMask(nullptr, 0.0);
        m_mapper->SetInputData(img);
    }

    // 初次绑定数据时，把切片平面放到图像中心，后续真正的位置再由 Cursor 状态驱动。
    double center[3];
    img->GetCenter(center);
    m_slicePlane->SetOrigin(center);
    m_slice->SetMapper(m_mapper);
}

void SliceStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    auto* image = vtkImageData::SafeDownCast(
        m_lastInput);
    if (!m_mapper || !image || !validityMask) {
        if (m_mapper && image) {
            (void)m_mapper->SetValidityMask(nullptr, 0.0);
            m_mapper->SetInputData(image);
        }
        return;
    }

    double range[2] = {};
    image->GetScalarRange(range);
    if (!m_mapper->SetValidityMask(validityMask, range[0])) {
        (void)m_mapper->SetValidityMask(nullptr, 0.0);
    }
    m_mapper->SetInputData(image);
}

std::array<int, 3> SliceStrategy::GetMaskWorkingDimensions() const
{
    std::array<int, 3> dimensions{};
    return m_mapper
        ? m_mapper->GetMaskWorkingDimensions()
        : dimensions;
}

bool SliceStrategy::GetMaskWorkingPixelsValid() const
{
    return m_mapper
        && m_mapper->GetMaskWorkingPixelsValid();
}

void SliceStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren); //
    m_renderer = ren;
}

void SliceStrategy::SetCrosshair(const double focusWorld[3],
    const double worldBounds[6],
    double safeOffset)
{
    if (!m_hLineSource || !m_vLineSource) return;
    const double physX = focusWorld[0];
    const double physY = focusWorld[1];
    const double physZ = focusWorld[2];

    // 十字线直接使用世界坐标构造，这样在模型矩阵变化后仍能与其它 3D/2D 视图保持同一套空间语义。
    if (m_orientation == Orientation::Top_down) {
        const double z = physZ + safeOffset;
        m_vLineSource->SetPoint1(physX, worldBounds[2], z);
        m_vLineSource->SetPoint2(physX, worldBounds[3], z);
        m_hLineSource->SetPoint1(worldBounds[0], physY, z);
        m_hLineSource->SetPoint2(worldBounds[1], physY, z);
    }
    else if (m_orientation == Orientation::Front_back) {
        const double y = physY + safeOffset;
        m_vLineSource->SetPoint1(physX, y, worldBounds[4]);
        m_vLineSource->SetPoint2(physX, y, worldBounds[5]);
        m_hLineSource->SetPoint1(worldBounds[0], y, physZ);
        m_hLineSource->SetPoint2(worldBounds[1], y, physZ);
    }
    else {
        const double x = physX + safeOffset;
        m_vLineSource->SetPoint1(x, physY, worldBounds[4]);
        m_vLineSource->SetPoint2(x, physY, worldBounds[5]);
        m_hLineSource->SetPoint1(x, worldBounds[2], physZ);
        m_hLineSource->SetPoint2(x, worldBounds[3], physZ);
    }

    m_vLineSource->Modified();
    m_hLineSource->Modified();
}

bool SliceStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if ((flags & UpdateFlags::WindowLevel) != UpdateFlags::None
        || (flags & UpdateFlags::Material) != UpdateFlags::None) {
        if (m_slice && m_slice->GetProperty()) {
            auto* property = m_slice->GetProperty();
            property->SetOpacity(1.0);
            property->SetColorWindow(params.windowLevel.windowWidth);
            property->SetColorLevel(params.windowLevel.windowCenter);
        }
    }

    const bool hasGeometryUpdate =
        (flags & UpdateFlags::Transform) != UpdateFlags::None
        || (flags & UpdateFlags::Cursor) != UpdateFlags::None;
    if (hasGeometryUpdate) {
        auto* image = vtkImageData::SafeDownCast(m_lastInput);
        auto* resliceMapper =
            vtkImageResliceMapper::SafeDownCast(m_mapper);
        if (!image || !resliceMapper || !resliceMapper->GetInput()) {
            return false;
        }
        if (image->GetMTime() != m_inputGeometryMTime) {
            m_inputGeometryMTime = image->GetMTime();
            std::copy_n(
                image->GetBounds(), m_inputBounds.size(),
                m_inputBounds.begin());
            std::copy_n(
                image->GetSpacing(), m_inputSpacing.size(),
                m_inputSpacing.begin());
            m_hasTransformCache = false;
        }

        const bool needsTransformCache = !m_hasTransformCache
            || m_modelMatrix != params.modelMatrix;
        if (needsTransformCache) {
            std::array<double, 6> worldBounds{};
            SetWorldBounds(
                m_inputBounds.data(), params.modelMatrix,
                worldBounds.data());
            if (!std::all_of(
                    worldBounds.begin(), worldBounds.end(),
                    [](const double value) {
                        return std::isfinite(value);
                    })) {
                return false;
            }
            std::array<double, 16> worldToInput{};
            vtkMatrix4x4::Invert(
                params.modelMatrix.data(), worldToInput.data());
            if (!std::all_of(
                    worldToInput.begin(), worldToInput.end(),
                    [](const double value) {
                        return std::isfinite(value);
                    })) {
                return false;
            }
            auto modelToWorld =
                vtkSmartPointer<vtkMatrix4x4>::New();
            modelToWorld->DeepCopy(params.modelMatrix.data());
            if (m_slice) m_slice->SetUserMatrix(modelToWorld);
            if (!m_mapper->SetWorldToInput(worldToInput)) return false;
            if (m_vLineActor) m_vLineActor->SetUserMatrix(nullptr);
            if (m_hLineActor) m_hLineActor->SetUserMatrix(nullptr);
            m_worldBounds = worldBounds;
            m_modelMatrix = params.modelMatrix;
            m_hasTransformCache = true;
            ++m_worldBoundsBuildCount;
            ++m_inverseBuildCount;
        }

        double worldNormal[3] = { 0.0, 0.0, 0.0 };
        if (m_orientation == Orientation::Top_down) {
            worldNormal[2] = 1.0;
        }
        else if (m_orientation == Orientation::Front_back) {
            worldNormal[1] = 1.0;
        }
        else {
            worldNormal[0] = 1.0;
        }
        auto* slicePlane = resliceMapper->GetSlicePlane();
        if (!slicePlane) {
            auto nextPlane = vtkSmartPointer<vtkPlane>::New();
            resliceMapper->SetSlicePlane(nextPlane);
            slicePlane = nextPlane;
        }
        slicePlane->SetOrigin(params.cursor.data());
        slicePlane->SetNormal(worldNormal);

        const double safeOffset = std::min({
            m_inputSpacing[0],
            m_inputSpacing[1],
            m_inputSpacing[2]
        });
        SetCrosshair(
            params.cursor.data(), m_worldBounds.data(), safeOffset);
    }

    if ((flags & UpdateFlags::Visibility) != UpdateFlags::None) {
        const int visibility =
            (params.visibilityMask & VisFlags::Crosshair) ? 1 : 0;
        if (m_vLineActor) m_vLineActor->SetVisibility(visibility);
        if (m_hLineActor) m_hLineActor->SetVisibility(visibility);
    }
    return true;
}

RenderEffectTarget SliceStrategy::GetRenderEffectTarget() const
{
    RenderEffectTarget target;
    if (m_mapper) {
        (void)m_mapper->GetEffectTarget(target);
    }
    return target;
}

void SliceStrategy::SetEffectBinding(RenderEffectBinding* binding)
{
    if (m_mapper) {
        (void)m_mapper->SetEffectBinding(binding);
    }
}
