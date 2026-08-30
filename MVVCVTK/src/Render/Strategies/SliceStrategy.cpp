#include "SliceStrategy.h"
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageProperty.h>
#include <algorithm>
#include <vtkTransform.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageMask.h>
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

protected:
    Mapper()
    {
        this->SliceMapper->Delete();
        this->SliceMapper = EffectImageSliceMapper::New();
    }

private:
    std::array<double, 16> m_worldToInput = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
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

    if (m_lastInput == data && m_mapper->GetInput()) {
        return;
    }
    m_lastInput = data;
    m_maskFilter = nullptr;

    m_mapper->SetInputData(img);

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
        m_maskFilter = nullptr;
        if (m_mapper && image) {
            m_mapper->SetInputData(image);
        }
        return;
    }

    double range[2] = {};
    image->GetScalarRange(range);
    m_maskFilter =
        vtkSmartPointer<vtkImageMask>::New();
    m_maskFilter->SetInputData(0, image);
    m_maskFilter->SetMaskInputData(validityMask);
    m_maskFilter->SetMaskedOutputValue(range[0]);
    m_maskFilter->NotMaskOff();
    m_mapper->SetInputConnection(
        m_maskFilter->GetOutputPort());
}

void SliceStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren); //
    m_renderer = ren;
    // 开启深度剥离，让 alpha<1 的像素正确透明（不影响不透明渲染）
    ren->SetUseDepthPeeling(1);
    ren->SetMaximumNumberOfPeels(4);
    ren->SetOcclusionRatio(0.0);
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
    // SliceStrategy 的状态同步核心分三段：
    // 1. WindowLevel/Material 更新图像显示参数
    // 2. Cursor/Transform 更新切片平面和十字线几何
    // 3. Visibility 控制十字线显隐

    // 窗宽/窗位或材质改变时，直接更新 vtkImageProperty 的 Window/Level；此处不重建 LUT。
    if (((flags & UpdateFlags::WindowLevel) != UpdateFlags::None) || ((flags & UpdateFlags::Material) != UpdateFlags::None))
    {
        if (m_slice && m_slice->GetProperty())
        {
            auto imgProp = m_slice->GetProperty();
            imgProp->SetOpacity(1.0);
            // imgProp->SetOpacity(params.material.opacity);
            imgProp->SetColorWindow(params.windowLevel.windowWidth);
            imgProp->SetColorLevel(params.windowLevel.windowCenter);
            // 切片无光照，不设 ambient/diffuse（与 vtkImageProperty 语义一致）
        }
    }

	if (((flags & UpdateFlags::Transform) != UpdateFlags::None) || ((flags & UpdateFlags::Cursor) != UpdateFlags::None))
    {
        auto resliceMapper = vtkImageResliceMapper::SafeDownCast(m_mapper);
        if (!resliceMapper || !resliceMapper->GetInput()) return false;

        double spacing[3], bounds[6];
        resliceMapper->GetInput()->GetSpacing(spacing);
        resliceMapper->GetInput()->GetBounds(bounds);
        double worldBounds[6] = { 0.0 };
        SetWorldBounds(bounds, params.modelMatrix, worldBounds);

        if (((flags & UpdateFlags::Transform) != UpdateFlags::None)) {
            auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            modelToWorldMatrix->DeepCopy(params.modelMatrix.data());
            if (m_slice) m_slice->SetUserMatrix(modelToWorldMatrix);
            std::array<double, 16> worldToInput = {};
            vtkMatrix4x4::Invert(
                params.modelMatrix.data(),
                worldToInput.data());
            (void)m_mapper->SetWorldToInput(worldToInput);
            if (m_vLineActor) m_vLineActor->SetUserMatrix(nullptr);
            if (m_hLineActor) m_hLineActor->SetUserMatrix(nullptr);
        }

        // 切片法线始终固定在视图朝向轴上；
        // 交互移动的是切片平面原点，不是法线方向本身。
        double worldNormal[3] = { 0.0, 0.0, 0.0 };
        if (m_orientation == Orientation::Top_down) worldNormal[2] = 1.0;
        else if (m_orientation == Orientation::Front_back) worldNormal[1] = 1.0;
        else worldNormal[0] = 1.0;

        auto slicePlane = resliceMapper->GetSlicePlane();
        if (!slicePlane) {
            slicePlane = vtkSmartPointer<vtkPlane>::New();
            resliceMapper->SetSlicePlane(slicePlane);
        }
        slicePlane->SetOrigin(params.cursor[0], params.cursor[1], params.cursor[2]);
        slicePlane->SetNormal(worldNormal[0], worldNormal[1], worldNormal[2]);

        const double safeOffset =
            std::min({ spacing[0], spacing[1], spacing[2] });
        SetCrosshair(params.cursor.data(), worldBounds, safeOffset);

    }

    if (((flags & UpdateFlags::Visibility) != UpdateFlags::None)) {
        const int vis = (params.visibilityMask & VisFlags::Crosshair) ? 1 : 0;
        if (m_vLineActor) m_vLineActor->SetVisibility(vis);
        if (m_hLineActor) m_hLineActor->SetVisibility(vis);
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
