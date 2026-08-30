#pragma once
#include <vtkTable.h>
#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>
#include <vtkWeakPointer.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 数据分析转换图表对象
class HistogramConverter {
public:
    bool SetBinCount(int binCount);
    vtkSmartPointer<vtkTable> GetOutputData(vtkSmartPointer<vtkImageData> input);
    std::optional<double> GetHistogramPercentile(vtkImageData* image, double quantile);
    std::uint64_t GetBuildCount() const;

    // 直方图转图片；filePath 为 UTF-8 路径。
    void ExportHistogram(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    // 内部复用：cache miss 执行 accumulate，随后返回不持有输入的频率缓存。
    vtkIdType* GetHistogramBuffer(vtkImageData* input, double outRange[2], double& outBinWidth);

    int m_binCount = 2048; // 默认 Bin 数量
    // 缓存可由多个 AppRuntime 共享，但不强持有大体数据。
    mutable std::mutex m_mutex;
    vtkWeakPointer<vtkImageData> m_cachedInput;
    std::vector<vtkIdType> m_cachedFrequencies;
    vtkMTimeType m_cachedMTime = 0;
    vtkMTimeType m_cachedScalarMTime = 0;
    int m_cachedBinCount = 0;
    std::array<double, 2> m_cachedRange{};
    double m_cachedBinWidth = 0.0;
    std::uint64_t m_buildCount = 0;
};
