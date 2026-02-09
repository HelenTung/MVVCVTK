#include "VolumeStrategy.h"
#include <vtkSmartVolumeMapper.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkImageResample.h>
#include <vtkCamera.h>
#include <vtkMatrix4x4.h>

VolumeStrategy::VolumeStrategy() {
    m_volume = vtkSmartPointer<vtkVolume>::New();
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
    m_volume->SetPickable(false); // 体渲染不可拾取
    m_cubeAxes->SetPickable(false); // 坐标轴不可拾取
}

void VolumeStrategy::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    auto img = vtkImageData::SafeDownCast(data);
    if (!img) return;

    // --- 降采样逻辑开始 ---
    vtkSmartPointer<vtkImageData> inputForMapper = img;
    int dims[3];
    img->GetDimensions(dims);

    // 目标最大分辨率
    const int targetDim = 766;

    // 仅当任意维度超过目标分辨率时才进行降采样
    if (dims[0] > targetDim || dims[1] > targetDim || dims[2] > targetDim) {
        auto resample = vtkSmartPointer<vtkImageResample>::New();
        resample->SetInputData(img);

        // 计算各轴缩放因子，将维度降至 766
        // vtkImageResample 会自动调整 Spacing，确保物理空间 Bounds 不变
        double factorX = static_cast<double>(targetDim) / static_cast<double>(dims[0]);
        double factorY = static_cast<double>(targetDim) / static_cast<double>(dims[1]);
        double factorZ = static_cast<double>(targetDim) / static_cast<double>(dims[2]);

        // 降采样到 766*766*766对所有轴进行映射
        resample->SetAxisMagnificationFactor(0, factorX);
        resample->SetAxisMagnificationFactor(1, factorY);
        resample->SetAxisMagnificationFactor(2, factorZ);

        resample->SetInterpolationModeToLinear(); // 使用线性插值平衡性能与质量
        resample->Update(); // 执行重采样

        inputForMapper = resample->GetOutput();
    }

    auto mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
    mapper->SetInputData(inputForMapper); // 使用处理后(或原始)的数据
    mapper->SetAutoAdjustSampleDistances(1); // 自动调整采样距离
    mapper->SetInteractiveUpdateRate(10.0);

    m_cubeAxes->SetBounds(inputForMapper->GetBounds());
    m_volume->SetMapper(mapper);
    if (!m_volume->GetProperty()) {
        auto volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
        volumeProperty->ShadeOn();
        volumeProperty->SetInterpolationTypeToLinear();
        m_volume->SetProperty(volumeProperty);
    }
}

void VolumeStrategy::Attach(vtkSmartPointer<vtkRenderer> ren) {
    ren->AddVolume(m_volume);
    ren->AddActor(m_cubeAxes);
    m_cubeAxes->SetCamera(ren->GetActiveCamera());
    ren->SetBackground(0.05, 0.05, 0.05); // 黑色背景
}

void VolumeStrategy::Detach(vtkSmartPointer<vtkRenderer> ren) {
    ren->RemoveVolume(m_volume);
    ren->RemoveActor(m_cubeAxes);
}

void VolumeStrategy::SetupCamera(vtkSmartPointer<vtkRenderer> ren) {
    ren->GetActiveCamera()->ParallelProjectionOff();
}

void VolumeStrategy::UpdateVisuals(const RenderParams& params, UpdateFlags flags)
{
    if (!m_volume || !m_volume->GetProperty()) return;

    // 响应 UpdateFlags::TF
    auto prop = m_volume->GetProperty();
    if ((int)flags & (int)UpdateFlags::TF || ((int)flags & (int)UpdateFlags::Material)) {
        // 构建 VTK 函数
        auto ctf = prop->GetRGBTransferFunction();
        auto otf = prop->GetScalarOpacity();

        if (!ctf)
        {
            ctf = vtkSmartPointer<vtkColorTransferFunction>::New();
            prop->SetColor(ctf);
        }

        if (!otf)
        {
            otf = vtkSmartPointer<vtkPiecewiseFunction>::New();
            prop->SetScalarOpacity(otf);
        }
        ctf->RemoveAllPoints();
        otf->RemoveAllPoints();

        double min = params.scalarRange[0];
        double max = params.scalarRange[1];
        double globalOpacity = params.material.opacity;

        for (const auto& node : params.tfNodes) {
            double val = min + node.position * (max - min);
            ctf->AddRGBPoint(val, node.r, node.g, node.b);
            otf->AddPoint(val, node.opacity * globalOpacity);
        }
        // 应用到底层
        prop->SetColor(ctf);
        prop->SetScalarOpacity(otf);
        prop->Modified();
    }

    // 响应 UpdateFlags::Material
    if ((int)flags & (int)UpdateFlags::Material) {
        prop->SetAmbient(params.material.ambient);
        prop->SetDiffuse(params.material.diffuse);
        prop->SetSpecular(params.material.specular);
        prop->SetSpecularPower(params.material.specularPower);

        if (params.material.shadeOn) prop->ShadeOn();
        else prop->ShadeOff();
    }

    // 响应变换矩阵
    if ((int)flags & (int)UpdateFlags::Transform) {
        vtkMatrix4x4* currentMat = m_volume->GetUserMatrix();
        if (!currentMat) {
            auto vtkMat = vtkSmartPointer<vtkMatrix4x4>::New();
            vtkMat->DeepCopy(params.modelMatrix.data());
            m_volume->SetUserMatrix(vtkMat);
        }
        else {
            currentMat->DeepCopy(params.modelMatrix.data());
        }
    }
}

vtkProp3D* VolumeStrategy::GetMainProp()
{
    if (!m_volume) return nullptr;
    else return m_volume;
}