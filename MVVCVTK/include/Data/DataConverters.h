#pragma once
#include <vtkTable.h>
#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkImageAccumulate.h>
#include <vtkType.h>
#include <string>

// 数据分析转换图表对象
class HistogramConverter {
private:
    int m_binCount = 2048; // 默认 Bin 数量
    vtkSmartPointer<vtkImageAccumulate> m_accumulate; // 持久化，支持流式复用
public:
    bool SetBinCount(int binCount);
    vtkSmartPointer<vtkTable> GetOutputData(vtkSmartPointer<vtkImageData> input);

    // 直方图转图片
    void ExportHistogram(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    // 内部复用：执行 accumulate 并返回频率指针，供 GetOutputData 与 ExportHistogram 共用同一管线。
    vtkIdType* GetHistogramBuffer(vtkSmartPointer<vtkImageData> input, double outRange[2], double& outBinWidth);
};
