#include "IsoSurfaceStrategy.h"
#include "Data/ImageProcessor.h"
#include <vtkProperty.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkClipPolyData.h>
#include <vtkImplicitFunction.h>
#include <vtkImageData.h>
#include <vtkType.h>

#include <algorithm>
#include <cmath>
#include <utility>

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
            || validityMask->GetScalarType()
                != VTK_UNSIGNED_CHAR
            || validityMask->GetNumberOfScalarComponents()
                != 1) {
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
        m_validityMask
            ->TransformPhysicalPointToContinuousIndex(
                point, continuousIndex);
        int extent[6] = {};
        m_validityMask->GetExtent(extent);
        int index[3] = {};
        for (int axis = 0; axis < 3; ++axis) {
            index[axis] = static_cast<int>(
                std::llround(continuousIndex[axis]));
            if (index[axis] < extent[axis * 2]
                || index[axis]
                    > extent[axis * 2 + 1]) {
                return -1.0;
            }
        }
        const auto* value =
            static_cast<const unsigned char*>(
                m_validityMask->GetScalarPointer(
                    index[0], index[1], index[2]));
        return value && *value != 0 ? 1.0 : -1.0;
    }

    void EvaluateGradient(
        double[3],
        double gradient[3]) override
    {
        gradient[0] = 0.0;
        gradient[1] = 0.0;
        gradient[2] = 0.0;
    }

private:
    vtkSmartPointer<vtkImageData> m_validityMask;
};

vtkStandardNewMacro(IsoSurfaceStrategy::MaskImplicit);


IsoSurfaceStrategy::IsoSurfaceStrategy() {
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_isoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
    m_maskFunc = vtkSmartPointer<MaskImplicit>::New();
    m_clip = vtkSmartPointer<vtkClipPolyData>::New();
    m_mapper = vtkSmartPointer<Mapper>::New();
    // predicate 直接读取 vertexMC；禁用 VBO Shift/Scale 才能保持 input-model 坐标。
    m_mapper->SetVBOShiftScaleMethod(vtkOpenGLPolyDataMapper::DISABLE_SHIFT_SCALE);
    // 初始绑定
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationToFlat();

    m_actor->SetPickable(false); // 等值面不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取

    // 静态数据
    m_actor->GetProperty()->SetInterpolationToFlat();
    m_isoFilter->ComputeNormalsOff();
    m_isoFilter->ComputeGradientsOff();
    m_clip->SetInputConnection(m_isoFilter->GetOutputPort());
    m_clip->SetClipFunction(m_maskFunc);
    m_clip->SetValue(0.0);
    m_clip->InsideOutOff();
    m_clip->GenerateClippedOutputOff();

    AttachProp(m_actor);
    AttachProp(m_cubeAxes);

}

IsoSurfaceStrategy::~IsoSurfaceStrategy() = default;

void IsoSurfaceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (m_lastInput == data) {
        return;
    }

    auto poly = vtkPolyData::SafeDownCast(data);
    if (poly) {
        // 如果上游已经给的是 mesh，则直接走 PolyData 路径，不再重复提等值面。
        m_lastInput = data;
        m_hasMask = false;
        m_mask = nullptr;
        m_isoRatio = 1.0;
        poly->GetCenter(m_dataCenter);
        m_mapper->SetInputData(poly);
        m_mapper->ScalarVisibilityOff();
        m_actor->SetMapper(m_mapper);
        m_cubeAxes->SetBounds(poly->GetBounds());

        // VG Style
        auto prop = m_actor->GetProperty();
        prop->SetColor(0.75, 0.75, 0.75); // VG 灰
        prop->SetAmbient(0.2);
        prop->SetDiffuse(0.8);
        prop->SetSpecular(0.15);      // 稍微增加一点高光
        prop->SetSpecularPower(15.0);
        prop->SetInterpolationToFlat();
        return;
    }

    // ImageData 路径始终从原始数据生成唯一等值面管线；
    // 交互来源不再拥有独立 producer，也不改 mapper 输入。
    auto img = vtkImageData::SafeDownCast(data);
    if (img) {
        const int* dimensions = img->GetDimensions();
        if (!dimensions) return;
        const int maxDimension = std::max(
            { dimensions[0], dimensions[1], dimensions[2] });
        constexpr int maxIsoDimension = 766;
        const double isoRatio = maxDimension > maxIsoDimension
            ? static_cast<double>(maxIsoDimension)
                / static_cast<double>(maxDimension)
            : 1.0;
        auto resample =
            ImageProcessor::CreateScaledImage(img, isoRatio);
        if (!resample) {
            return;
        }

        m_lastInput = data;
        m_hasMask = false;
        m_mask = nullptr;
        m_isoRatio = isoRatio;
        img->GetCenter(m_dataCenter);
        m_resample = std::move(resample);
        m_isoFilter->SetInputConnection(
            m_resample->GetOutputPort());

        m_currentIsoValue = 0.0;
        m_isoFilter->SetValue(0, m_currentIsoValue);
        m_mapper->SetInputConnection(m_isoFilter->GetOutputPort());
        m_mapper->ScalarVisibilityOff();
        m_cubeAxes->SetBounds(img->GetBounds());
    }

}

bool IsoSurfaceStrategy::SetMapperInput()
{
    if (!m_mapper) {
        return false;
    }
    if (m_hasMask) {
        if (!m_clip) {
            return false;
        }
        m_mapper->SetInputConnection(m_clip->GetOutputPort());
        return true;
    }
    if (!m_isoFilter) {
        return false;
    }
    m_mapper->SetInputConnection(m_isoFilter->GetOutputPort());
    return true;
}

void IsoSurfaceStrategy::SetInputMask(
    vtkSmartPointer<vtkImageData> validityMask)
{
    if (!vtkImageData::SafeDownCast(m_lastInput)
        || !validityMask) {
        m_hasMask = false;
        m_mask = nullptr;
        (void)SetMapperInput();
        return;
    }

    auto mask = ImageProcessor::CreateScaledMask(
        validityMask, m_isoRatio);
    if (!mask) {
        return;
    }
    mask->Update();
    if (!m_maskFunc->SetMask(mask->GetOutput())) {
        return;
    }
    m_mask = std::move(mask);
    m_hasMask = true;
    (void)SetMapperInput();
}

void IsoSurfaceStrategy::AttachRenderer(vtkSmartPointer<vtkRenderer> ren) {
    BaseVisualStrategy::AttachRenderer(ren);
    m_renderer = ren;
    m_cubeAxes->SetCamera(ren->GetActiveCamera());

}

bool IsoSurfaceStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    if (!m_actor) return false;
    auto prop = m_actor->GetProperty();

    // 等值面策略主要消费三类状态：
    // 1. Material 控制表面光照和透明度
    // 2. IsoValue 控制几何提取阈值
    // 3. Transform / Visibility 控制空间摆放与辅助元素显隐

    // 响应 UpdateFlags::Material
    if (((flags & UpdateFlags::Material) != UpdateFlags::None)) {

        // 设置光照参数
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        // 设置几何体透明度
        prop->SetOpacity(params.material.opacity);
        // 设置着色方式,开启光照
        if (params.material.isShadeOn) prop->SetInterpolationToPhong();
        else prop->SetInterpolationToFlat();
    }

    // 交互来源只改变窗口刷新调度；等值面几何始终来自原始数据 producer。
    if ((flags & UpdateFlags::IsoValue) != UpdateFlags::None) {
        m_currentIsoValue = params.isoValue;
        if (m_isoFilter && m_isoFilter->GetInput()
            && m_isoFilter->GetValue(0) != m_currentIsoValue) {
            m_isoFilter->SetValue(0, m_currentIsoValue);
        }
    }

    // 响应 UpdateFlags::Transform
    if (((flags & UpdateFlags::Transform) != UpdateFlags::None)) {
        Set3DPropsTransform(params.modelMatrix);
    }

    if (((flags & UpdateFlags::Visibility) != UpdateFlags::None)) {
        if (m_cubeAxes)
            m_cubeAxes->SetVisibility(
                (params.visibilityMask & VisFlags::Ruler) ? 1 : 0);
    }
    return true;
}

vtkProp3D* IsoSurfaceStrategy::GetMainProp()
{
    return m_actor;
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

void IsoSurfaceStrategy::SetEffectBinding(RenderEffectBinding* binding)
{
    if (m_mapper) {
        m_mapper->SetEffectBinding(binding);
    }
}
