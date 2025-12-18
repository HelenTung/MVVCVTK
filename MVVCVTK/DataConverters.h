#pragma once
#include "AppInterfaces.h"

// 将 ImageData 转换为 PolyData (等值面提取)
class IsoSurfaceConverter : public AbstractDataConverter<vtkImageData, vtkPolyData> {
private:
    double m_isoValue = 0.0;
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkPolyData> Process(vtkSmartPointer<vtkImageData> input) override;
};
