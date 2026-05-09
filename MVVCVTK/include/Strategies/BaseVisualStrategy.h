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

class BaseVisualStrategy :public AbstractVisualStrategy
{
protected:
	std::vector<vtkSmartPointer<vtkProp>> m_managedProps; // 当前策略管理的 VTK 组件列表，供 Attach/Detach 统一处理
    vtkSmartPointer<vtkImageResample> m_resampleFilter;
    void SetManagedProp(vtkSmartPointer<vtkProp> prop)
	{
		if (prop)
			m_managedProps.push_back(prop);
	}

public:
	virtual ~BaseVisualStrategy() = default;
    void SetRendererAttached(vtkSmartPointer<vtkRenderer> renderer) override {
		if (!renderer) return;
		for (auto& prop : m_managedProps) 
				renderer->AddViewProp(prop);
	}
    void SetRendererDetached(vtkSmartPointer<vtkRenderer> renderer) override {
		if (!renderer) return;
		for (auto& prop : m_managedProps)
			renderer->RemoveViewProp(prop);
	}

protected:
    void Set3DPropsTransform(const std::array<double, 16>& matrixData) {
        for (auto prop : m_managedProps) {
            auto prop3D = vtkProp3D::SafeDownCast(prop);
          if (!prop3D) continue; // 仅对 3D prop 应用模型矩阵，跳过文本等 2D prop
			auto matrix = prop3D->GetUserMatrix();
            if (!matrix)
            {
                vtkSmartPointer<vtkMatrix4x4> mat = vtkSmartPointer<vtkMatrix4x4>::New();
				mat->DeepCopy(matrixData.data());
				prop3D->SetUserMatrix(mat);
            }
            else
            {
                matrix->DeepCopy(matrixData.data());
            }
		}
    }

    vtkAlgorithmOutput* GetDownsampledOutputPort(vtkImageData* input, int targetDim = 766)
    {
		if (!input) return nullptr;
        m_resampleFilter = ImageProcessor::GetDownsampledImage(input, targetDim);
		return m_resampleFilter ? m_resampleFilter->GetOutputPort() : nullptr;
    }

    void SetImageBoundsClamped(int& x, int& y, int& z, const int dims[3]) {
        if (!dims) return;
        x = std::max(0, std::min(x, dims[0] - 1));
        y = std::max(0, std::min(y, dims[1] - 1));
        z = std::max(0, std::min(z, dims[2] - 1));
    }
};
