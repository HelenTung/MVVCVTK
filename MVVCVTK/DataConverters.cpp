#include "DataConverters.h"
//#include <vtkMarchingCubes.h>
#include <vtkFlyingEdges3D.h>
#include <opencv2/opencv.hpp>
#include <vtkFloatArray.h>
#include <vtkIntArray.h>

void IsoSurfaceConverter::SetParameter(const std::string& key, double value) {
    if (key == "IsoValue") m_isoValue = value;
}

vtkSmartPointer<vtkPolyData> IsoSurfaceConverter::Process(vtkSmartPointer<vtkImageData> input) {
	// 使用 FlyingEdges3D 算法提取等值面
    auto mc = vtkSmartPointer<vtkFlyingEdges3D>::New();
    mc->SetInputData(input);
    mc->ComputeNormalsOn();
    mc->SetValue(0, m_isoValue);
    // 是否做简化三角面片？

    mc->Update(); // 立即执行计算
    return mc->GetOutput();
}

void HistogramConverter::SetParameter(const std::string& key, double value)
{
    if (key == "BinCount") m_binCount = static_cast<int>(value);
}

vtkSmartPointer<vtkTable> HistogramConverter::Process(vtkSmartPointer<vtkImageData> input)
{
	if (!input) return nullptr;

	// 获取图像数据的维度
    int dims[3] = { 0 };
	input->GetDimensions(dims);

	// 计算总体素数
    size_t totalVoxels = static_cast<size_t>(dims[0]) * dims[1] * dims[2];

	// 获取指向图像数据的指针
    float* rawData = static_cast<float*>(input->GetScalarPointer());
    if (!rawData) return nullptr;

	// 将数据转换为 OpenCV Mat 格式 (1行，totalVoxels列)
    cv::Mat src(1, totalVoxels, CV_32F, rawData);

	// 获取数据范围
    int histSize = m_binCount;
    double value[2];
    input->GetScalarRange(value);
    float range[] = { (float)value[0], (float)value[1]}; // 动态范围
    const float* histRange = { range };

	// 计算直方图的区间范围
    cv::Mat cvHist;
    cv::calcHist(&src, 1, 0, cv::Mat(), cvHist, 1, &histSize, &histRange, true, false);
    
	// 构建 VTK Table 对象
    auto table = vtkSmartPointer<vtkTable>::New();
	auto maxVal = static_cast<float>(value[1]);
	auto minVal = static_cast<float>(value[0]);
    
    // 创建两列：X轴 (Intensity/Density), Y轴 (Frequency)
    auto colX = vtkSmartPointer<vtkFloatArray>::New();
    colX->SetName("Intensity");
    colX->SetNumberOfValues(histSize);

    auto colY = vtkSmartPointer<vtkFloatArray>::New(); // 原始频率
    colY->SetName("Frequency");
    colY->SetNumberOfValues(histSize);

    auto colLogY = vtkSmartPointer<vtkFloatArray>::New(); // 对数频率 (方便 UI 绘图)
    colLogY->SetName("LogFrequency");
    colLogY->SetNumberOfValues(histSize);

    float step = (maxVal - minVal) / histSize;

    for (int i = 0; i < histSize; i++) {
        float binVal = cvHist.at<float>(i);

        // 填充 X 轴数值
        colX->SetValue(i, minVal + i * step);

        // 填充 Y 轴频率
        colY->SetValue(i, binVal);

        // 填充 Log Y (避免 log(0))
        colLogY->SetValue(i, std::log(binVal + 1.0f));
    }

    table->AddColumn(colX);
    table->AddColumn(colY);
    table->AddColumn(colLogY);

    return table;
}


void HistogramConverter::SaveHistogramImage(vtkSmartPointer<vtkImageData> input, const std::string& filePath)
{
    if (!input) return;

    // 1. 准备数据
    int dims[3] = { 0 };
    input->GetDimensions(dims);
    size_t totalVoxels = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    float* rawData = static_cast<float*>(input->GetScalarPointer());
    if (!rawData) return;

    // 2. 计算直方图 (使用 OpenCV)
    cv::Mat src(1, totalVoxels, CV_32F, rawData);
    int histSize = m_binCount; // 默认 2048
    double rangeVal[2];
    input->GetScalarRange(rangeVal);
    float range[] = { (float)rangeVal[0], (float)rangeVal[1] };
    const float* histRange = { range };

    cv::Mat cvHist;
    cv::calcHist(&src, 1, 0, cv::Mat(), cvHist, 1, &histSize, &histRange, true, false);

    // --- 对数变换 ---
    std::vector<float> logHist(histSize);
    float maxLogVal = 0.0f;
    // 记录最大值的索引，用于辅助定位红线
    int maxIdx = 0;

    for (int i = 0; i < histSize; ++i) {
        float val = cvHist.at<float>(i);
        float logVal = std::log(val + 1.0f);
        logHist[i] = logVal;
        if (logVal > maxLogVal) {
            maxLogVal = logVal;
            maxIdx = i;
        }
    }

    // 创建画布 (800x600)
    int imgW = 800;
    int imgH = 600;
    cv::Mat histImage(imgH, imgW, CV_8UC3);

    // --- 绘制背景渐变 (保持 VG 的 X 轴灰度映射感) ---
    cv::Mat gradientRow(1, imgW, CV_8UC3);
    for (int i = 0; i < imgW; ++i) {
        int v = (i * 255) / imgW;
        gradientRow.at<cv::Vec3b>(0, i) = cv::Vec3b(v, v, v);
    }
    cv::repeat(gradientRow, imgH, 1, histImage);

    // --- 构建填充多边形 ---
    std::vector<cv::Point> points;
    points.push_back(cv::Point(0, imgH)); // 起点左下

    for (int i = 0; i < histSize; i++)
    {
        int x = (int)((double)i / histSize * imgW);
        // Y 坐标映射: 留出顶部 10% 边距
        int y_height = (int)(logHist[i] / maxLogVal * (imgH * 0.9));
        int y = imgH - y_height;
        points.push_back(cv::Point(x, y));
    }
    points.push_back(cv::Point(imgW, imgH)); // 终点右下

    // --- 填充直方图 (VG 风格: 浅中灰色) ---
    std::vector<std::vector<cv::Point>> polys = { points };
    // 使用 (128, 128, 128) 灰色，比之前的深灰更像 VG
    cv::fillPoly(histImage, polys, cv::Scalar(128, 128, 128));

    // 保存
    cv::imwrite(filePath, histImage);
    std::cout << "VG-Style Histogram saved to: " << filePath << std::endl;
}
