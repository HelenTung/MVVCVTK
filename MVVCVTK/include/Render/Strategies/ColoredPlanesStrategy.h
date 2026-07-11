#pragma once
#include "BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>

class ColoredPlanesStrategy : public BaseVisualStrategy {
public:
    ColoredPlanesStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetVisualState(const RenderParams& params, UpdateFlags flags);
    int GetPlaneAxis(vtkActor* actor) override;

private:
    // cursorWorld 位于世界坐标；modelMatrix 按 input model -> world 解释，用于重算世界 AABB。
    void SetAllPositions(const double cursorWorld[3], const std::array<double, 16>& modelMatrix);
    // 输出布局固定为 [minX,maxX,minY,maxY,minZ,maxZ]，表示变换后 8 个角点的世界 AABB。
    void SetWorldBounds(const std::array<double, 16>& modelMatrix, double worldBounds[6]) const;

    // 三组数组共享索引：0=X/Left_right(YZ)，1=Y/Front_back(XZ)，2=Z/Top_down(XY)。
    // actor/source 均由策略强持有；actor 还登记进 m_managedProps，source 由各 actor mapper 的管线引用。
    vtkSmartPointer<vtkActor> m_planeActors[3];
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3];
    // 最近一次有效 vtkImageData 的强引用和身份缓存；输入对象原地修改时不能仅靠该字段识别内容变化。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 输入 dimensions-1 的 X/Y/Z 快照；当前位置计算尚未消费，不能作为 plane 位置或边界真源。
    int m_maxIndices[3] = { 0, 0, 0 };
    // 输入模型坐标 AABB，布局 [minX,maxX,minY,maxY,minZ,maxZ]；SetWorldBounds 从它派生世界 AABB。
    double m_bounds[6] = { 0.0 };
    // 输入图像 X/Y/Z 物理 spacing 快照；当前平面跨度仍以 m_bounds 为真源。
    double m_spacing[3] = { 0 };
    // 输入模型坐标 X/Y/Z origin；只用于首次建立三张参考平面的轴向锚点。
    double m_origin[3] = { 0 };
};
