#include "DataConverters.h"
//#include <vtkMarchingCubes.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageAccumulate.h>
#include <vtkFloatArray.h>
#include <vtkIntArray.h>
#include <filesystem>
#include <vtkImageWriter.h>
#include <vtkJPEGWriter.h>
#include <vtkPNGWriter.h>

void IsoSurfaceConverter::SetParameter(const std::string& key, double value) {
    if (key == "IsoValue") m_isoValue = value;
}

vtkSmartPointer<vtkPolyData> IsoSurfaceConverter::Process(vtkSmartPointer<vtkImageData> input) {
	// 使用 FlyingEdges3D 算法提取等值面
    auto mc = vtkSmartPointer<vtkFlyingEdges3D>::New();
    mc->SetInputData(input);
    mc->ComputeNormalsOn();
    mc->SetValue(0, m_isoValue);

    mc->Update(); // 立即执行计算
    return mc->GetOutput();
}

void HistogramConverter::SetParameter(const std::string& key, double value)
{
    if (key == "BinCount") m_binCount = static_cast<int>(value);
}

vtkSmartPointer<vtkTable> HistogramConverter::Process(vtkSmartPointer<vtkImageData> input) {
    if (!input) return nullptr;
    double range[2];
    input->GetScalarRange(range);

    auto accumulate = vtkSmartPointer<vtkImageAccumulate>::New();
    accumulate->SetInputData(input);
    accumulate->SetComponentExtent(0, m_binCount - 1, 0, 0, 0, 0);
    accumulate->SetComponentOrigin(range[0], 0, 0);
    double binWidth = (range[1] - range[0]) / static_cast<double>(m_binCount);
    accumulate->SetComponentSpacing(binWidth > 0 ? binWidth : 1.0, 0, 0);
    accumulate->Update();

    vtkImageData* output = accumulate->GetOutput();
    long long* frequencies = static_cast<long long*>(output->GetScalarPointer());

    auto table = vtkSmartPointer<vtkTable>::New();
    auto colX = vtkSmartPointer<vtkFloatArray>::New(); colX->SetName("Intensity");
    auto colY = vtkSmartPointer<vtkFloatArray>::New(); colY->SetName("Frequency");
    auto colLogY = vtkSmartPointer<vtkFloatArray>::New(); colLogY->SetName("LogFrequency");

    for (int i = 0; i < m_binCount; i++) {
        float val = static_cast<float>(frequencies[i]);
        colX->InsertNextValue(range[0] + i * binWidth);
        colY->InsertNextValue(val);
        colLogY->InsertNextValue(std::log(val + 1.0f));
    }
    table->AddColumn(colX); table->AddColumn(colY); table->AddColumn(colLogY);
    return table;
}


void HistogramConverter::SaveHistogramImage(vtkSmartPointer<vtkImageData> input, const std::string& filePath) {
    if (!input) return;

    // 直方图频率
    double range[2];
    input->GetScalarRange(range);
    auto acc = vtkSmartPointer<vtkImageAccumulate>::New();
    acc->SetInputData(input);
    acc->SetComponentExtent(0, m_binCount - 1, 0, 0, 0, 0);
    acc->SetComponentOrigin(range[0], 0, 0);
    double binWidth = (range[1] - range[0]) / static_cast<double>(m_binCount);
    acc->SetComponentSpacing(binWidth > 0 ? binWidth : 1.0, 0, 0);
    acc->Update();

    long long* freqs = static_cast<long long*>(acc->GetOutput()->GetScalarPointer());
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