#include "DataConverters.h"
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
            int binIdx = (x * binCount) / W;
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
