#pragma once
#include "AppInterfaces.h"
#include <vector>

class RawVolumeDataManager : public AbstractDataManager {
private:
    vtkSmartPointer<vtkImageData> m_vtkImage;      // VTK 包装对象
    int m_dims[3] = { 0, 0, 0 };
    double m_spacing = 0.02125;

public:
    RawVolumeDataManager();
    bool LoadData(const std::string& filePath) override;
    vtkSmartPointer<vtkImageData> GetVtkImage() const override;
};
