#pragma once
#include "AppInterfaces.h"
#include <vtkCubeAxesActor.h>
#include <map>

class MedicalVizService : public AbstractAppService {
private:
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache;
public:
    MedicalVizService();

    // 业务入口：加载文件
    void LoadFile(const std::string& path);

    // 业务功能：切换到体渲染
    void ShowVolume();

    // 业务功能：切换到等值面
    void ShowIsoSurface();

    // 业务功能：切换到 Z 轴切片
    void ShowSliceAxial();

private:
    void UpdateAxes();

	// 获取或创建策略
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);
    void ClearCache();
};
