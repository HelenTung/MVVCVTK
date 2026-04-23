#pragma once
#include "AppInterfaces.h"
#include <vtkTable.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyData.h>
#include <vtkImageData.h>
#include <vtkAlgorithm.h>
#include <vtkSmartPointer.h>
#include <vtkImageAccumulate.h>
#include <vtkQuadricDecimation.h>

// 将 ImageData 转换为 PolyData (等值面提取)
class IsoSurfaceConverter : public AbstractDataConverter<vtkImageData, vtkPolyData> {
private:
    double m_isoValue = 0.0;
    vtkSmartPointer<vtkFlyingEdges3D> m_filter;
	vtkSmartPointer<vtkQuadricDecimation> m_decimator; // 可选的网格简化器
public:
    IsoSurfaceConverter() : m_filter(vtkSmartPointer<vtkFlyingEdges3D>::New()), m_decimator(vtkSmartPointer<vtkQuadricDecimation>::New()) {
        m_filter->ComputeNormalsOff();   // 法线关闭，极大提升提取速度
		m_filter->ComputeGradientsOff(); // 梯度关闭，进一步提升速度，但可能导致某些边缘细节丢失
        //m_decimator->SetTargetReduction(0.8); // 减少 80% 的三角面，但不损失边缘精度

    }
    void SetParameter(const std::string& key, double value) override;
    // 流式接口直接返回 OutputPort，上层直接 SetInputConnection
    vtkSmartPointer<vtkPolyData> GetOutputData(vtkSmartPointer<vtkImageData> input) override;
    vtkAlgorithmOutput* GetOutputPort() { return m_filter->GetOutputPort(); }
};

// 数据分析转换图表对象
class HistogramConverter : public AbstractDataConverter<vtkImageData, vtkTable> {
private:
    int m_binCount = 2048; // 默认 Bin 数量
    vtkSmartPointer<vtkImageAccumulate> m_accumulate; // 持久化，支持流式复用
public:
    void SetParameter(const std::string& key, double value) override;
    vtkSmartPointer<vtkTable> GetOutputData(vtkSmartPointer<vtkImageData> input) override;

    // 直方图转图片
    void SetHistogramImageSaved(vtkSmartPointer<vtkImageData> input, const std::string& filePath);

private:
    // 内部复用：执行 accumulate 并返回频率指针，避免 Process 与 SaveHistogramImage 重复计算
    long long* GetHistogramBuffer(vtkSmartPointer<vtkImageData> input, double outRange[2], double& outBinWidth);
};