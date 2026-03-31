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
	void RegisterProp(vtkSmartPointer<vtkProp> prop)
	{
		if (prop)
			m_managedProps.push_back(prop);
	}

public:
	virtual ~BaseVisualStrategy() = default;
	void Attach(vtkSmartPointer<vtkRenderer> renderer) {
		if (!renderer) return;
		for (auto& prop : m_managedProps) 
				renderer->AddViewProp(prop);
	}
	void Detach(vtkSmartPointer<vtkRenderer> renderer) {
		if (!renderer) return;
		for (auto& prop : m_managedProps)
			renderer->RemoveViewProp(prop);
	}

protected:
    void RebuildGrayscaleLUT(vtkLookupTable* lut, const RenderParams& params) {
        if (!lut) return;

        const double ww = params.windowLevel.windowWidth;
        const double wc = params.windowLevel.windowCenter;
        const double lo = wc - ww * 0.5;
        const double hi = wc + ww * 0.5;

        const double minVal = params.scalarRange[0];
        const double maxVal = params.scalarRange[1];
        if (maxVal - minVal <= 0.0) return;

        const int nTable = 256;
        lut->SetNumberOfTableValues(nTable);
        lut->SetTableRange(minVal, maxVal);

        for (int i = 0; i < nTable; i++) {
            const double scalar = minVal + (maxVal - minVal) * (double(i) / (nTable - 1));
            double gray;
            if (scalar <= lo) gray = 0.0;
            else if (scalar >= hi) gray = 1.0;
            else              gray = (scalar - lo) / ww;

            lut->SetTableValue(i, gray, gray, gray, params.material.opacity);
        }
        lut->Build();
    }

    void ApplyTransformTo3DProps(const std::array<double, 16>& matrixData) {
        for (auto prop : m_managedProps) {
            auto prop3D = vtkProp3D::SafeDownCast(prop);
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
		m_resampleFilter = ImageProcessor::ApplyDownsampling(input, targetDim);
		return m_resampleFilter ? m_resampleFilter->GetOutputPort() : nullptr;
    }

    void JungeToImageBounds(int& x, int& y, int& z, const int dims[3]) {
        if (!dims) return;
        x = std::max(0, std::min(x, dims[0] - 1));
        y = std::max(0, std::min(y, dims[1] - 1));
        z = std::max(0, std::min(z, dims[2] - 1));
    }
};
