#pragma once
#include "AppInterfaces.h"
#include <vtkTable.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyData.h>
#include <vtkImageData.h>
#include <vtkAlgorithm.h>
#include <vtkSmartPointer.h>
#include <vtkImageAccumulate.h>

// 将 ImageData 转换为 PolyData (等值面提取)
class IsoSurfaceConverter : public AbstractDataConverter<vtkImageData, vtkPolyData> {
private:
    double m_isoValue = 0.0;
    vtkSmartPointer<vtkFlyingEdges3D> m_filter;
public:
    IsoSurfaceConverter() : m_filter(vtkSmartPointer<vtkFlyingEdges3D>::New()) {}
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkPolyData> Process(vtkSmartPointer<vtkImageData> input) override;
    // 流式接口直接返回 OutputPort，上层直接 SetInputConnection
    vtkAlgorithmOutput* GetOutputPort() { return m_filter->GetOutputPort(); }
};

// 数据分析转换图表对象
class HistogramConverter : public AbstractDataConverter<vtkImageData, vtkTable> {
private:
    int m_binCount = 2048; // 默认 Bin 数量
    vtkSmartPointer<vtkImageAccumulate> m_accumulate; // 持久化，支持流式复用
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkTable> Process(vtkSmartPointer<vtkImageData> input) override;

    // 直方图转图片
    void SaveHistogramImage(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    // 内部复用：执行 accumulate 并返回频率指针，避免 Process 与 SaveHistogramImage 重复计算
    long long* ComputeHistogram(vtkSmartPointer<vtkImageData> input, double outRange[2], double& outBinWidth);
};