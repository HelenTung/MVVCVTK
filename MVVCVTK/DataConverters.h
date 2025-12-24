#pragma once
#include "AppInterfaces.h"
#include <vtkTable.h>

// 将 ImageData 转换为 PolyData (等值面提取)
class IsoSurfaceConverter : public AbstractDataConverter<vtkImageData, vtkPolyData> {
private:
    double m_isoValue = 0.0;
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkPolyData> Process(vtkSmartPointer<vtkImageData> input) override;
};

// 数据分析转换图表对象
class HistogramConverter : public AbstractDataConverter<vtkImageData, vtkTable> {
private:
    int m_binCount = 2048; // 默认 Bin 数量
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkTable> Process(vtkSmartPointer<vtkImageData> input) override;

    // 直方图转图片
    void SaveHistogramImage(vtkSmartPointer<vtkImageData> input, const std::string& filePath);
};