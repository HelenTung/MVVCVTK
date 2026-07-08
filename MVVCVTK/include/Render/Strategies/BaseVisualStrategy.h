#pragma once
#include "AppInterfaces.h"
#include "ImageProcessor.h"
#include <vtkProp.h>
#include <vtkProp3D.h>
#include <vtkMatrix4x4.h>
#include <vtkLookupTable.h>
#include <vtkImageResample.h>
#include <vector>
#include <algorithm>

class BaseVisualStrategy : public AbstractVisualStrategy {
protected:
    std::vector<vtkSmartPointer<vtkProp>> m_managedProps; // 当前策略管理的 VTK 组件列表，供 Attach/Detach 统一处理
    vtkSmartPointer<vtkImageResample> m_resampleFilter; // 需要降采样时缓存 filter，保证输出端口生命周期稳定

    void AttachProp(vtkSmartPointer<vtkProp> prop)
    {
        if (prop) {
            m_managedProps.push_back(prop);
        }
    }

public:
    virtual ~BaseVisualStrategy() = default;

    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        // 基类只处理“把当前策略拥有的 prop 全部挂到 renderer 上”，
        // 具体背景色、相机或附加 VTK 关系由派生类继续补充。
        for (auto& prop : m_managedProps) {
            renderer->AddViewProp(prop);
        }
    }

    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) {
            return;
        }
        for (auto& prop : m_managedProps) {
            renderer->RemoveViewProp(prop);
        }
    }

protected:
    void Set3DPropsTransform(const std::array<double, 16>& modelToWorldMatrixData)
    {
        // 对策略拥有的 3D prop 批量下发同一份模型矩阵，
        // 保证 actor、axes 等附属对象始终跟随同一个世界变换。
        for (auto prop : m_managedProps) {
            auto prop3D = vtkProp3D::SafeDownCast(prop);
            if (!prop3D) {
                continue; // 仅对 3D prop 应用模型矩阵，跳过文本等 2D prop
            }
            auto userMatrix = prop3D->GetUserMatrix();
            if (!userMatrix) {
                vtkSmartPointer<vtkMatrix4x4> modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
                modelToWorldMatrix->DeepCopy(modelToWorldMatrixData.data());
                prop3D->SetUserMatrix(modelToWorldMatrix);
            }
            else {
                userMatrix->DeepCopy(modelToWorldMatrixData.data());
            }
        }
    }

    vtkAlgorithmOutput* GetDownsampledOutputPort(vtkImageData* input, int targetDim = 766)
    {
        if (!input) {
            return nullptr;
        }
        // 降采样集中放在基类，避免 3D 策略各自复制同样的数据降采样逻辑。
        m_resampleFilter = ImageProcessor::GetDownsampledImage(input, targetDim);
        return m_resampleFilter ? m_resampleFilter->GetOutputPort() : nullptr;
    }

    void ClampImageBounds(int& x, int& y, int& z, const int dims[3])
    {
        if (!dims) {
            return;
        }
        x = std::max(0, std::min(x, dims[0] - 1));
        y = std::max(0, std::min(y, dims[1] - 1));
        z = std::max(0, std::min(z, dims[2] - 1));
    }
};
