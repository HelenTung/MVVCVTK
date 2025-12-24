#include "DataConverters.h"
//#include <vtkMarchingCubes.h>
#include <vtkFlyingEdges3D.h>

void IsoSurfaceConverter::SetParameter(const std::string& key, double value) {
    if (key == "IsoValue") m_isoValue = value;
}

vtkSmartPointer<vtkPolyData> IsoSurfaceConverter::Process(vtkSmartPointer<vtkImageData> input) {
	// 使用 FlyingEdges3D 算法提取等值面
    auto mc = vtkSmartPointer<vtkFlyingEdges3D>::New();
    mc->SetInputData(input);
    mc->ComputeNormalsOn();
    mc->SetValue(0, m_isoValue);
    // 是否做简化三角面片？

    mc->Update(); // 立即执行计算
    return mc->GetOutput();
}
