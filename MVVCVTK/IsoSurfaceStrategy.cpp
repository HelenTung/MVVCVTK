#include "IsoSurfaceStrategy.h"
#include <vtkProperty.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyDataNormals.h>
#include <thread>

IsoSurfaceStrategy::IsoSurfaceStrategy() {
    m_actor = vtkSmartPointer<vtkLODActor>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_isoFilter = vtkSmartPointer<vtkFlyingEdges3D>::New();
    m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();

    // 初始绑定
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetInterpolationToPhong();

    m_actor->SetPickable(false); // 等值面不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取

    // 静态数据
    m_actor->SetNumberOfCloudPoints(50000);
    m_actor->GetProperty()->SetInterpolationToPhong();
}

void IsoSurfaceStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto poly = vtkPolyData::SafeDownCast(data);
    if (poly) {
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
        prop->SetInterpolationToGouraud();
        return;
    }

    // 作为 ImageData (需要��时计算)
    auto img = vtkImageData::SafeDownCast(data);
    if (img) {
        m_isoFilter->SetInputData(img);
        m_isoFilter->ComputeNormalsOff();
        m_isoFilter->ComputeGradientsOff(); 

        // 设定初始阈值
        double range[2];
        img->GetScalarRange(range);
        double initialVal = range[0] + (range[1] - range[0]) * 0.2;
        m_isoFilter->SetValue(0, initialVal);

        // 使用 Connection，VTK 会自动管理更新
        m_mapper->SetInputConnection(m_isoFilter->GetOutputPort());
        m_mapper->ScalarVisibilityOff();
        m_cubeAxes->SetBounds(img->GetBounds());
    }
}

void IsoSurfaceStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddActor(m_actor);
    ren->AddActor(m_cubeAxes);
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
    ren->SetBackground(0.1, 0.15, 0.2); // 蓝色调背景
}

void IsoSurfaceStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveActor(m_actor);
    ren->RemoveActor(m_cubeAxes);
}

void IsoSurfaceStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    // 3D 模式必须是透视投影
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void IsoSurfaceStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (!m_actor) return;
    auto prop = m_actor->GetProperty();

    // 响应 UpdateFlags::Material
    if ((int)flags & (int)UpdateFlags::Material) {

        // 设置光照参数
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        // 设置几何体透明度
        prop->SetOpacity(params.material.opacity);
        // 设置着色方式,开启光照
        if (params.material.shadeOn) prop->SetInterpolationToPhong();
        else prop->SetInterpolationToFlat();
    }

    if (((int)flags & (int)UpdateFlags::IsoValue)) {
        if (m_isoFilter && m_isoFilter->GetInput()) {
            // 只有当阈值真的改变时才重新计算
            if (m_isoFilter->GetValue(0) != params.isoValue) {
                m_isoFilter->SetValue(0, params.isoValue);
				m_mapper->Modified(); // 标记 Mapper 需要更新
            }
        }
    }

    // 响应 UpdateFlags::Transform
    if ((int)flags & (int)UpdateFlags::Transform) {
        vtkMatrix4x4* currentMat = m_actor->GetUserMatrix();
        if (!currentMat) {
            // 如果本来没有矩阵，才创建新的
            auto vtkMat = vtkSmartPointer<vtkMatrix4x4>::New();
            vtkMat->DeepCopy(params.modelMatrix.data());
            m_actor->SetUserMatrix(vtkMat);
        }
        else {
            // 如果已有矩阵，直接覆写数据，保持指针地址不变
            currentMat->DeepCopy(params.modelMatrix.data());
        }
    }

    if (HasFlag(flags, UpdateFlags::Visibility)) {
        if (m_cubeAxes)
            m_cubeAxes->SetVisibility(
                (params.visibilityMask & VisFlags::RulerAxes) ? 1 : 0);
    }
}

vtkProp3D* IsoSurfaceStrategy::GetMainProp()
{
    return m_actor;
}