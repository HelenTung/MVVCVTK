#include "DataConverters.h"
#include <vtkImageAccumulate.h>
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <filesystem>
#include <vtkImageWriter.h>
#include <vtkJPEGWriter.h>
#include <vtkPNGWriter.h>
#include <vtkImageData.h>

void HistogramConverter::SetParameter(const std::string& key, double value)
{
    if (key == "BinCount" && value > 0.0) m_binCount = static_cast<int>(value);
}

vtkSmartPointer<vtkTable> HistogramConverter::GetOutputData(vtkSmartPointer<vtkImageData> input) {
    if (!input) return nullptr;
    double range[2], binWidth;
    vtkIdType* frequencies = GetHistogramBuffer(input, range, binWidth);
    if (!frequencies) return nullptr;

    auto table = vtkSmartPointer<vtkTable>::New();
    auto colX = vtkSmartPointer<vtkFloatArray>::New(); colX->SetName("Intensity");
    auto colY = vtkSmartPointer<vtkFloatArray>::New(); colY->SetName("Frequency");
    auto colLogY = vtkSmartPointer<vtkFloatArray>::New(); colLogY->SetName("LogFrequency");

    // 预分配避免 InsertNextValue 反复扩容
    colX->SetNumberOfTuples(m_binCount);
    colY->SetNumberOfTuples(m_binCount);
    colLogY->SetNumberOfTuples(m_binCount);

    for (int i = 0; i < m_binCount; i++) {
        float val = static_cast<float>(frequencies[i]);
        colX->SetValue(i, static_cast<float>(range[0] + i * binWidth));
        colY->SetValue(i, val);
        colLogY->SetValue(i, std::log(val + 1.0f));
    }
    table->AddColumn(colX); table->AddColumn(colY); table->AddColumn(colLogY);
    return table;
}


void HistogramConverter::SetHistogramImageSaved(vtkSmartPointer<vtkImageData> input, const std::string& filePath) {
    if (!input) return;
    double range[2], binWidth;
    // 复用 ComputeHistogram，不重复计算
    vtkIdType* freqs = GetHistogramBuffer(input, range, binWidth);
    if (!freqs) return;

    std::vector<float> logHist(m_binCount);
    float maxLog = 0.0f;
    for (int i = 0; i < m_binCount; ++i) {
        logHist[i] = std::log(static_cast<float>(freqs[i]) + 1.0f);
        if (logHist[i] > maxLog) maxLog = logHist[i];
    }

    //  800x600 绘图画布大小
    int W = 800, H = 600;
    auto canvas = vtkSmartPointer<vtkImageData>::New();
    canvas->SetDimensions(W, H, 1);
    canvas->AllocateScalars(VTK_UNSIGNED_CHAR, 3);
    unsigned char* ptr = static_cast<unsigned char*>(canvas->GetScalarPointer());

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int binIdx = (x * m_binCount) / W;
            int h_limit = static_cast<int>((logHist[binIdx] / (maxLog > 0 ? maxLog : 1.0f)) * H * 0.9);
            unsigned char* pix = ptr + (y * W + x) * 3;

            if (y < h_limit) { // 填充直方图 (y=0在底部)
                pix[0] = pix[1] = pix[2] = 128;
            }
            else { // 背景渐变
                pix[0] = pix[1] = pix[2] = static_cast<unsigned char>((x * 255) / W);
            }
        }
    }

    std::string ext = std::filesystem::path(filePath).extension().string();
    vtkSmartPointer<vtkImageWriter> writer;
    if (ext == ".png" || ext == ".PNG") writer = vtkSmartPointer<vtkPNGWriter>::New();
    else writer = vtkSmartPointer<vtkJPEGWriter>::New();

    writer->SetFileName(filePath.c_str());
    writer->SetInputData(canvas);
    writer->Write();
}

vtkIdType* HistogramConverter::GetHistogramBuffer(vtkSmartPointer<vtkImageData> input, double outRange[2], double& outBinWidth)
{
    if (!input || !input->GetPointData() || !input->GetPointData()->GetScalars() || m_binCount <= 0) {
        outRange[0] = 0.0;
        outRange[1] = 0.0;
        outBinWidth = 0.0;
        return nullptr;
    }

    input->GetScalarRange(outRange);
    // 流式连接：input 没变时 VTK pipeline 不会重复计算
    if (!m_accumulate)
		m_accumulate = vtkSmartPointer<vtkImageAccumulate>::New();
    m_accumulate->SetInputData(input);
    m_accumulate->SetComponentExtent(0, m_binCount - 1, 0, 0, 0, 0);
    m_accumulate->SetComponentOrigin(outRange[0], 0, 0);
    outBinWidth = (outRange[1] - outRange[0]) / static_cast<double>(m_binCount);
    m_accumulate->SetComponentSpacing(outBinWidth > 0 ? outBinWidth : 1.0, 0, 0);
    m_accumulate->Update();

    if (!m_accumulate->GetOutput() || !m_accumulate->GetOutput()->GetPointData() || !m_accumulate->GetOutput()->GetPointData()->GetScalars()) {
        return nullptr;
    }

    return static_cast<vtkIdType*>(m_accumulate->GetOutput()->GetScalarPointer());
}
