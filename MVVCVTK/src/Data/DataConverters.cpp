#include "DataConverters.h"
#include "App/Services/AppPorts.h"
#include "Platform/Path.h"
#include <vtkImageAccumulate.h>
#include <vtkDoubleArray.h>
#include <vtkIdTypeArray.h>
#include <vtkPointData.h>
#include <filesystem>
#include <vtkImageWriter.h>
#include <vtkJPEGWriter.h>
#include <vtkPNGWriter.h>
#include <vtkImageData.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <vector>
#include "Data/DataPayloads.h"
#include "Support/ImagePeakEstimator.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

namespace {
struct AutoIsoState final {
    std::atomic<bool> isCancelled{false};
    mutable std::mutex mutex;
    std::optional<AutoIsoResult> result;
};
template<class Scalar> double ReadPeakScalar(const void* values, const std::size_t index)
{
    Scalar value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(values) + index * sizeof(Scalar), sizeof(Scalar));
    return static_cast<double>(value);
}
double (*GetPeakReader(const ImageValueType type))(const void*, std::size_t)
{
    switch (type) {
    case ImageValueType::Int8: return ReadPeakScalar<std::int8_t>;
    case ImageValueType::UInt8: return ReadPeakScalar<std::uint8_t>;
    case ImageValueType::Int16: return ReadPeakScalar<std::int16_t>;
    case ImageValueType::UInt16: return ReadPeakScalar<std::uint16_t>;
    case ImageValueType::Int32: return ReadPeakScalar<std::int32_t>;
    case ImageValueType::UInt32: return ReadPeakScalar<std::uint32_t>;
    case ImageValueType::Int64: return ReadPeakScalar<std::int64_t>;
    case ImageValueType::UInt64: return ReadPeakScalar<std::uint64_t>;
    case ImageValueType::Float32: return ReadPeakScalar<float>;
    case ImageValueType::Float64: return ReadPeakScalar<double>;
    default: return nullptr;
    }
}
}

class AutoIsoTask::Impl final {
public:
    explicit Impl(std::shared_ptr<AutoIsoState> value) : state(std::move(value)) {}
    std::shared_ptr<AutoIsoState> state;
};
AutoIsoTask::AutoIsoTask(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
AutoIsoTask::~AutoIsoTask() { m_impl->state->isCancelled.store(true, std::memory_order_release); }
std::optional<AutoIsoResult> AutoIsoTask::GetResult() const
{
    const std::lock_guard<std::mutex> lock(m_impl->state->mutex);
    return m_impl->state->result;
}

class HistogramConverter::PeakCache final {
public:
    struct Entry final {
        DataRevisionRef revision;
        // 键引用真正的冻结缓冲；重新包装 payload 不重扫，地址重用不误命中。
        std::weak_ptr<const ImageScalarStorage> values;
        std::weak_ptr<const std::vector<std::uint8_t>> validity;
        std::array<int, 3> dimensions;
        std::array<int, 6> extent;
        ImageValueType valueType;
        std::size_t components;
        std::shared_ptr<AutoIsoState> state;
        std::weak_ptr<AutoIsoTask> request;
    };
    std::vector<Entry> entries;
    std::uint64_t builds = 0;
};

TaskAdmissionResult HistogramConverter::StartAutoIso(const DataRevisionRef& revision,
    std::shared_ptr<const ImageGrid3DPayload> source,
    const std::function<TaskAdmissionResult(RenderLaneWork)>& onStart,
    std::shared_ptr<AutoIsoTask>& task)
{
    if (!source || !onStart) return TaskAdmissionResult::InvalidRequest;
    const std::lock_guard<std::mutex> lock(m_peakMutex);
    try {
        if (!m_peakCache) m_peakCache = std::make_shared<PeakCache>();
        auto& entries = m_peakCache->entries;
        for (auto entry = entries.begin(); entry != entries.end();) {
            if (entry->values.expired()) { entry = entries.erase(entry); continue; }
            const auto& values = source->GetValues();
            const auto& validity = source->GetValidityMask();
            if (entry->revision == revision && entry->values.lock().get() == values.get()
                && !entry->values.owner_before(values) && !values.owner_before(entry->values)
                && entry->validity.lock().get() == validity.get()
                && !entry->validity.owner_before(validity) && !validity.owner_before(entry->validity)
                && entry->dimensions == source->GetGeometry().dimensions
                && entry->extent == source->GetGeometry().extent
                && entry->valueType == source->GetValueType()
                && entry->components == source->GetComponentCount()) {
                if (auto request = entry->request.lock()) {
                    task = std::move(request);
                    return TaskAdmissionResult::Accepted;
                }
                const std::lock_guard<std::mutex> resultLock(entry->state->mutex);
                if (entry->state->result && !entry->state->result->isCancelled) {
                    task = std::shared_ptr<AutoIsoTask>(new AutoIsoTask(std::make_unique<AutoIsoTask::Impl>(entry->state)));
                    entry->request = task;
                    return TaskAdmissionResult::Accepted;
                }
                entry = entries.erase(entry);
                continue;
            }
            ++entry;
        }
        // 只限制统计缓存，不限制源数据规模或业务发布。活动请求由候选自身保活。
        constexpr std::size_t peakCacheLimit = 8;
        if (entries.size() == peakCacheLimit) entries.erase(entries.begin());
        auto state = std::make_shared<AutoIsoState>();
        auto request = std::shared_ptr<AutoIsoTask>(new AutoIsoTask(std::make_unique<AutoIsoTask::Impl>(state)));
        entries.reserve(peakCacheLimit);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        RenderLaneWork work([source, state, deadline](TaskStopToken stop) {
            AutoIsoResult result;
            const auto stopping = [&] { return stop.GetIsStopped() || state->isCancelled.load(std::memory_order_acquire); };
            const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
            const auto cancelled = [&] { return stopping() || expired(); };
            try {
                std::optional<ImagePeakEstimate> estimate;
                ImagePeakInput input;
                input.dimensions = source->GetGeometry().dimensions;
                if (source->GetComponentCount() == 1 && source->GetValues()) {
                    input.values = source->GetValues()->data();
                    input.readScalar = GetPeakReader(source->GetValueType());
                    input.validity = source->GetValidityMask() ? source->GetValidityMask()->data() : nullptr;
                }
                const auto failure = ImagePeakEstimator::BuildEstimate(input, cancelled, estimate, result.message);
                result.isCancelled = stopping();
                if (!result.isCancelled && failure == ImagePeakFailure::None && estimate)
                    result.isoValue = estimate->isoValue;
            } catch (const std::exception& error) {
                result.message = error.what();
                result.isCancelled = stopping();
            } catch (...) {
                result.message = "Image peak estimation failed.";
                result.isCancelled = stopping();
            }
            if (expired()) {
                result.isoValue.reset();
                result.message = "Automatic peak estimation reached its deadline.";
            }
            if (!result.isCancelled && !result.isoValue)
                std::cerr << "[AutoIso] 自动峰值不可用，沿用范围中点: " << result.message << '\n';
            {
                const std::lock_guard<std::mutex> resultLock(state->mutex);
                state->result = std::move(result);
            }
            return true;
        });
        const auto admission = onStart(std::move(work));
        if (admission != TaskAdmissionResult::Accepted) return admission;
        entries.push_back({revision, source->GetValues(), source->GetValidityMask(),
            source->GetGeometry().dimensions, source->GetGeometry().extent,
            source->GetValueType(), source->GetComponentCount(), state, request});
        ++m_peakCache->builds;
        task = std::move(request);
        return TaskAdmissionResult::Accepted;
    } catch (...) {
        return TaskAdmissionResult::Unavailable;
    }
}

std::uint64_t HistogramConverter::GetPeakBuildCount() const
{
    const std::lock_guard<std::mutex> lock(m_peakMutex);
    return m_peakCache ? m_peakCache->builds : 0;
}

bool HistogramConverter::SetBinCount(int binCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (binCount <= 0) {
        return false;
    }
    if (m_binCount == binCount) return true;
    m_binCount = binCount;
    m_cachedInput = nullptr;
    m_cachedFrequencies.clear();
    m_cachedMTime = 0;
    m_cachedScalarMTime = 0;
    m_cachedBinCount = 0;
    return true;
}

std::uint64_t HistogramConverter::GetBuildCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buildCount;
}

vtkSmartPointer<vtkTable> HistogramConverter::GetOutputData(vtkSmartPointer<vtkImageData> input) {
    if (!input) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    double range[2], binWidth;
    vtkIdType* frequencies = GetHistogramBuffer(input, range, binWidth);
    if (!frequencies) return nullptr;

    auto table = vtkSmartPointer<vtkTable>::New();
    auto colX = vtkSmartPointer<vtkDoubleArray>::New(); colX->SetName("Intensity");
    auto colY = vtkSmartPointer<vtkIdTypeArray>::New(); colY->SetName("Frequency");
    auto colLogY = vtkSmartPointer<vtkDoubleArray>::New(); colLogY->SetName("LogFrequency");

    // 预分配避免 InsertNextValue 反复扩容
    colX->SetNumberOfTuples(m_binCount);
    colY->SetNumberOfTuples(m_binCount);
    colLogY->SetNumberOfTuples(m_binCount);

    for (int i = 0; i < m_binCount; i++) {
        const vtkIdType frequency = frequencies[i];
        colX->SetValue(i, range[0] + static_cast<double>(i) * binWidth);
        colY->SetValue(i, frequency);
        colLogY->SetValue(i, std::log(static_cast<double>(frequency) + 1.0));
    }
    table->AddColumn(colX); table->AddColumn(colY); table->AddColumn(colLogY);
    return table;
}


void HistogramConverter::ExportHistogram(vtkSmartPointer<vtkImageData> input, const std::string& filePath) {
    if (!input) return;
    int binCount = 0;
    std::vector<double> logHist;
    double maxLog = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        double range[2], binWidth;
        // 复用频率缓存；复制对数表后释放锁，不把文件写出纳入缓存临界区。
        vtkIdType* freqs = GetHistogramBuffer(input, range, binWidth);
        if (!freqs) return;
        binCount = m_binCount;
        logHist.resize(static_cast<std::size_t>(binCount));
        for (int i = 0; i < binCount; ++i) {
            logHist[static_cast<std::size_t>(i)] =
                std::log(static_cast<double>(freqs[i]) + 1.0);
            if (logHist[static_cast<std::size_t>(i)] > maxLog) {
                maxLog = logHist[static_cast<std::size_t>(i)];
            }
        }
    }

    //  800x600 绘图画布大小
    int W = 800, H = 600;
    auto canvas = vtkSmartPointer<vtkImageData>::New();
    canvas->SetDimensions(W, H, 1);
    canvas->AllocateScalars(VTK_UNSIGNED_CHAR, 3);
    unsigned char* ptr = static_cast<unsigned char*>(canvas->GetScalarPointer());

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const auto binIdx = static_cast<std::size_t>((static_cast<std::int64_t>(x) * binCount) / W);
            int h_limit = static_cast<int>((logHist[binIdx] / (maxLog > 0.0 ? maxLog : 1.0)) * H * 0.9);
            unsigned char* pix = ptr + (y * W + x) * 3;

            if (y < h_limit) { // 填充直方图 (y=0在底部)
                pix[0] = pix[1] = pix[2] = 128;
            }
            else { // 背景渐变
                pix[0] = pix[1] = pix[2] = static_cast<unsigned char>((x * 255) / W);
            }
        }
    }

    const std::filesystem::path outputPath = PlatformPath::GetNativePath(filePath);
    if (outputPath.empty()) {
        return;
    }
    const auto parentDir = outputPath.parent_path();
    if (!parentDir.empty()) {
        try {
            std::filesystem::create_directories(parentDir);
        }
        catch (...) {
            return;
        }
    }
    std::string ext = PlatformPath::GetUtf8Path(outputPath.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    vtkSmartPointer<vtkImageWriter> writer;
    if (ext == ".png") writer = vtkSmartPointer<vtkPNGWriter>::New();
    else writer = vtkSmartPointer<vtkJPEGWriter>::New();

    const std::string vtkFileName = PlatformPath::GetUtf8Path(outputPath);
    writer->SetFileName(vtkFileName.c_str());
    writer->SetInputData(canvas);
    writer->Write();
}

std::optional<double> HistogramConverter::GetHistogramPercentile(
    vtkImageData* image,
    double quantile)
{
    if (!std::isfinite(quantile) || quantile < 0.0 || quantile > 1.0) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    double range[2] = { 0.0, 0.0 };
    double binWidth = 0.0;
    vtkIdType* frequencies = GetHistogramBuffer(image, range, binWidth);
    if (!frequencies) {
        return std::nullopt;
    }
    if (quantile == 0.0 || range[0] == range[1]) {
        return range[0];
    }
    if (quantile == 1.0) {
        return range[1];
    }

    long double sampleCount = 0.0L;
    for (int i = 0; i < m_binCount; ++i) {
        sampleCount += static_cast<long double>(frequencies[i]);
    }
    if (sampleCount <= 0.0L) {
        return std::nullopt;
    }

    const long double targetRank =
        std::ceil(static_cast<long double>(quantile) * sampleCount);
    long double currentRank = 0.0L;
    for (int i = 0; i < m_binCount; ++i) {
        currentRank += static_cast<long double>(frequencies[i]);
        if (currentRank >= targetRank) {
            const double estimate =
                range[0] + static_cast<double>(i) * binWidth;
            return std::clamp(estimate, range[0], range[1]);
        }
    }
    return range[1];
}

vtkIdType* HistogramConverter::GetHistogramBuffer(vtkImageData* input, double outRange[2], double& outBinWidth)
{
    if (!input || !input->GetPointData() || !input->GetPointData()->GetScalars() || m_binCount <= 0) {
        outRange[0] = 0.0;
        outRange[1] = 0.0;
        outBinWidth = 0.0;
        return nullptr;
    }

    auto* inputScalars = input->GetPointData()->GetScalars();
    if (m_cachedInput.GetPointer() == input
        && m_cachedMTime == input->GetMTime()
        && m_cachedScalarMTime == inputScalars->GetMTime()
        && m_cachedBinCount == m_binCount
        && m_cachedFrequencies.size()
            == static_cast<std::size_t>(m_binCount)) {
        outRange[0] = m_cachedRange[0];
        outRange[1] = m_cachedRange[1];
        outBinWidth = m_cachedBinWidth;
        return m_cachedFrequencies.data();
    }

    input->GetScalarRange(outRange);
    if (!std::isfinite(outRange[0]) || !std::isfinite(outRange[1])
        || outRange[1] < outRange[0]) {
        outBinWidth = 0.0;
        return nullptr;
    }

    const double rangeWidth = outRange[1] - outRange[0];
    double binSpacing = 1.0;
    if (rangeWidth > 0.0 && m_binCount == 1) {
        outBinWidth = std::nextafter(
            rangeWidth, std::numeric_limits<double>::infinity());
        binSpacing = outBinWidth;
    }
    else if (rangeWidth > 0.0) {
        outBinWidth = rangeWidth / static_cast<double>(m_binCount - 1);
        binSpacing = outBinWidth;
    }
    else {
        outBinWidth = 0.0;
    }

    // cache miss 才扫描整卷；频率表复制后立即释放 VTK pipeline，避免缓存滞留旧体数据。
    auto accumulate = vtkSmartPointer<vtkImageAccumulate>::New();
    accumulate->SetInputData(input);
    accumulate->SetComponentExtent(
        0, m_binCount - 1, 0, 0, 0, 0);
    accumulate->SetComponentOrigin(outRange[0], 0, 0);
    accumulate->SetComponentSpacing(binSpacing, 0, 0);
    accumulate->Update();

    auto* output = accumulate->GetOutput();
    auto* outputScalars = output && output->GetPointData()
        ? output->GetPointData()->GetScalars() : nullptr;
    auto* frequencies = outputScalars
        ? static_cast<vtkIdType*>(output->GetScalarPointer()) : nullptr;
    if (!frequencies) {
        return nullptr;
    }

    m_cachedFrequencies.assign(
        frequencies, frequencies + m_binCount);
    m_cachedInput = input;
    m_cachedMTime = input->GetMTime();
    m_cachedScalarMTime = inputScalars->GetMTime();
    m_cachedBinCount = m_binCount;
    m_cachedRange = { outRange[0], outRange[1] };
    m_cachedBinWidth = outBinWidth;
    ++m_buildCount;
    return m_cachedFrequencies.data();
}
