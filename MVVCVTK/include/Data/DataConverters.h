#pragma once
#include <vtkTable.h>
#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>
#include <vtkWeakPointer.h>
#include "Data/DataGraphTypes.h"
#include "Render/Internal/RenderResourceCoordinator.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class ImageGrid3DPayload;
enum class TaskAdmissionResult : std::uint8_t;

// 仓内数据准备结果；不进入 HostAPI/FeatureSPI 或安装头集合。
struct AutoIsoResult final {
    std::optional<double> isoValue;
    bool isCancelled = false;
    std::string message;
};

// 最后一个候选释放请求时取消计算；worker 只保留纯数据状态。
class AutoIsoTask final {
public:
    ~AutoIsoTask();
    std::optional<AutoIsoResult> GetResult() const;
private:
    friend class HistogramConverter;
    class Impl;
    explicit AutoIsoTask(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> m_impl;
};

// 数据分析转换图表对象
class HistogramConverter {
public:
    bool SetBinCount(int binCount);
    vtkSmartPointer<vtkTable> GetOutputData(vtkSmartPointer<vtkImageData> input);
    std::optional<double> GetHistogramPercentile(vtkImageData* image, double quantile);
    std::uint64_t GetBuildCount() const;
    // 仅内部 owner 接纳；onStart 同步调用固定执行器的入队入口，不回调业务、
    // 不重入 converter。Accepted 可立即已有结果，后续 GetResult 只读领取；
    // QueueFull 不生成请求，调用者等待现有任务的工作通知后重试。
    TaskAdmissionResult StartAutoIso(const DataRevisionRef& revision,
        std::shared_ptr<const ImageGrid3DPayload> source,
        const std::function<TaskAdmissionResult(RenderLaneWork)>& onStart,
        std::shared_ptr<AutoIsoTask>& task);
    std::uint64_t GetPeakBuildCount() const;

    // 直方图转图片；filePath 为 UTF-8 路径。
    void ExportHistogram(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    class PeakCache;
    mutable std::mutex m_peakMutex;
    std::shared_ptr<PeakCache> m_peakCache;
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
