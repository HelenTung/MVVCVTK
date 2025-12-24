#include "DataConverters.h"
#include <vtkMarchingCubes.h>

void IsoSurfaceConverter::SetParameter(const std::string& key, double value) {
    if (key == "IsoValue") m_isoValue = value;
}

vtkSmartPointer<vtkPolyData> IsoSurfaceConverter::Process(vtkSmartPointer<vtkImageData> input) {
    auto mc = vtkSmartPointer<vtkMarchingCubes>::New();
    mc->SetInputData(input);
    mc->ComputeNormalsOn();
    mc->SetValue(0, m_isoValue);
    mc->Update(); // 立即执行计算
    return mc->GetOutput();
}
