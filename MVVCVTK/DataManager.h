#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <mutex>

class RawVolumeDataManager : public AbstractDataManager {
private:
    mutable std::mutex m_mutex;
    vtkSmartPointer<vtkImageData> m_vtkImage;      // VTK 包装对象
    int m_dims[3] = { 0, 0, 0 };
    double m_spacing = 0.02125;

public:
    RawVolumeDataManager();
    bool LoadData(const std::string& filePath) override;
    vtkSmartPointer<vtkImageData> GetVtkImage() const override;
};


class TiffVolumeDataManager : public AbstractDataManager {
private:
    mutable std::mutex m_mutex;
    vtkSmartPointer<vtkImageData> m_vtkImage;

public:
    TiffVolumeDataManager();
    // 实现加载接口
    bool LoadData(const std::string& filePath) override;
    // 实现数据获取接口
    vtkSmartPointer<vtkImageData> GetVtkImage() const override;
};
