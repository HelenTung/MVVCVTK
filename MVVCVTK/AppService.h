#pragma once
#include "AppInterfaces.h"
#include <vtkCubeAxesActor.h>
#include <map>
#include "AppState.h"
#include <vtkTable.h>

class MedicalVizService : public AbstractAppService {
private:
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
    // 引用共享状态
    std::shared_ptr<SharedInteractionState> m_sharedState;
public:
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);

    // 业务入口：加载文件
    void LoadFile(const std::string& path);

    // 业务功能：切换到体渲染
    void ShowVolume();

    // 业务功能：切换到等值面
    void ShowIsoSurface();

	// 业务功能：切换到2d轴切片
    void ShowSlice(VizMode sliceMode);

    // 切换到 3D 多切面模式
    void Show3DPlanes(VizMode renderMode);

	// 更新交互值 (如切片位置)
    void UpdateInteraction(int value);

	// 状态变更通知回调
    void OnStateChanged();

	// 访问共享状态
    std::shared_ptr<SharedInteractionState> GetSharedState() {
        return m_sharedState;
    }

    // 返回轴向
    int GetPlaneAxis(vtkActor* actor); 

    // 业务功能：获取当前数据的直方图统计
    vtkSmartPointer<vtkTable> GetHistogramData(int binCount = 2048);

	// 业务功能：保存当前数据的直方图为图片
	void SaveHistogramImage(const std::string& filePath, int binCount = 2048);
private:
	// 更新坐标轴显示
    void UpdateAxes();
    // 将全局坐标重置为图像中心
    void ResetCursorCenter();
    // 获取或创建策略
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void ClearCache();

};