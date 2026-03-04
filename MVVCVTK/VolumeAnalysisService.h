#pragma once
// =====================================================================
// VolumeAnalysisService.h
//
// 职责：纯数据分析，与渲染管线完全解耦。
//   • 直方图计算
//   • 统计数据导出（直方图图片等）
//
// =====================================================================
#include "AppInterfaces.h"
#include "DataConverters.h"
#include <vtkTable.h>
#include <string>
#include <memory>

class VolumeAnalysisService {
private:
    std::shared_ptr<AbstractDataManager> m_dataManager;

public:
    explicit VolumeAnalysisService(std::shared_ptr<AbstractDataManager> dataMgr)
        : m_dataManager(std::move(dataMgr))
    {
    }

    // 计算直方图数据表（binCount 建议 1024~4096，视精度需求而定）
    vtkSmartPointer<vtkTable> GetHistogramData(int binCount = 2048) const {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return nullptr;
        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", static_cast<double>(binCount));
        return converter->Process(m_dataManager->GetVtkImage());
    }

    // 保存直方图为 PNG 图片
    void SaveHistogramImage(const std::string& filePath, int binCount = 2048) const {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", static_cast<double>(binCount));
        converter->SaveHistogramImage(m_dataManager->GetVtkImage(), filePath);
    }
};