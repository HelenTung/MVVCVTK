#pragma once
#include "AppInterfaces.h"
#include <vtkTable.h>
#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkImageAccumulate.h>
#include <vtkType.h>

// 数据分析转换图表对象
class HistogramConverter : public AbstractDataConverter<vtkImageData, vtkTable> {
private:
    int m_binCount = 2048; // 默认 Bin 数量
    vtkSmartPointer<vtkImageAccumulate> m_accumulate; // 持久化，支持流式复用
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkTable> GetOutputData(vtkSmartPointer<vtkImageData> input) override;

    // 直方图转图片
    void ExportHistogram(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    // 内部复用：执行 accumulate 并返回频率指针，避免 Process 与 ExportHistogram 重复计算
    vtkIdType* GetHistogramBuffer(vtkSmartPointer<vtkImageData> input, double outRange[2], double& outBinWidth);
};