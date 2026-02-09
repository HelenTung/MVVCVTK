#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "DataConverters.h"
#include <vtkTable.h>
#include <map>
#include <vtkTransform.h>

/**
 * @class VolumeAnalysisService
 * @brief 专注于数据分析的服务，负责直方图计算、统计导出等，与渲染解耦。
 */
class VolumeAnalysisService {
private:
    std::shared_ptr<AbstractDataManager> m_dataManager;

public:
    explicit VolumeAnalysisService(std::shared_ptr<AbstractDataManager> dataMgr)
        : m_dataManager(dataMgr) {
    }

    // 计算直方图数据
    vtkSmartPointer<vtkTable> GetHistogramData(int binCount = 2048) {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return nullptr;

        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", (double)binCount);
        return converter->Process(m_dataManager->GetVtkImage());
    }

    // 保存直方图图片
    void SaveHistogramImage(const std::string& filePath, int binCount = 2048) {
        if (!m_dataManager || !m_dataManager->GetVtkImage()) return;

        auto converter = std::make_shared<HistogramConverter>();
        converter->SetParameter("BinCount", (double)binCount);
        converter->SaveHistogramImage(m_dataManager->GetVtkImage(), filePath);
    }
};

class MedicalVizService : public AbstractInteractiveService,
public std::enable_shared_from_this<MedicalVizService>{
private:
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    std::shared_ptr<SharedInteractionState> m_sharedState;

public:
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);
    void Initialize(vtkSmartPointer<vtkRenderWindow> win, vtkSmartPointer<vtkRenderer> ren) override;

    // --- 核心渲染业务 ---
    void LoadFile(const std::string& path);
    void ShowVolume();
    void ShowIsoSurface();
    void ShowSlice(VizMode sliceMode);
    void Show3DPlanes(VizMode renderMode);

    // --- 交互业务,具体实现 ---
    int GetPlaneAxis(vtkActor* actor) override;
    void UpdateInteraction(int delta) override;
    void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) override;
	void ProcessPendingUpdates() override;
    std::array<int, 3> GetCursorPosition() override;
    void SetInteracting(bool val) override;

    std::shared_ptr<SharedInteractionState> GetSharedState() {
        return m_sharedState;
    }

    // 设置模型的位置、旋转、缩放
    void TransformModel(double translate[3], double rotate[3], double scale[3]);
    // 重置模型变换
    void ResetModelTransform();
    // 坐标转换核心功能
    // 世界坐标 (渲染窗口中的坐标) -> 模型坐标 (原始数据的物理坐标)
    void WorldToModel(const double worldPos[3], double modelPos[3]);
    // 模型坐标 -> 世界坐标
    void ModelToWorld(const double modelPos[3], double worldPos[3]);
    vtkProp3D* GetMainProp() override;
    void SyncModelMatrix(vtkMatrix4x4* mat) override;
public:
	// 参数设置接口
    void SetLuxParams(double ambient, double diffuse, double specular, double power, bool shadeOn = false);
    void SetOpacity(double opacity);
    void SetIsoThreshold(double val);
    void SetTransferFunction(const std::vector<TFNode>& nodes);


    private:
    void OnStateChanged();
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void ResetCursorCenter();
    void ClearCache();
};