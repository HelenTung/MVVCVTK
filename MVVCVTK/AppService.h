#pragma once
#include "AppInterfaces.h"
#include <vtkCubeAxesActor.h>
#include <map>

class MedicalVizService : public AbstractAppService {
private:
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
public:
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr);

    // 业务入口：加载文件
    void LoadFile(const std::string& path);

    // 业务功能：切换到体渲染
    void ShowVolume();

    // 业务功能：切换到等值面
    void ShowIsoSurface();

	// 业务功能：切换到2d轴切片
    void ShowSliceAxial();

	// 更新交互值 (如切片位置)
    void UpdateInteraction(int value);

	// 更新切片朝向
    void UpdateSliceOrientation(Orientation orient);

private:
    void UpdateAxes();

    // 获取或创建策略
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void ClearCache();
};