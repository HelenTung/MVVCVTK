#pragma once
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkLookupTable.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>

#include <array>
#include <cstdint>

// --- 策略 C: 2D 切片 (MPR) ---
// index = z*dx*dy + y*dx + x
class SliceStrategy : public BaseVisualStrategy {
public:
    SliceStrategy(Orientation orient);
    ~SliceStrategy() override;

    // [Public] 抽象接口实现
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetInputMask(
        vtkSmartPointer<vtkImageData> validityMask) override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer);
    bool SetVisualState(
        const RenderParams& params,
        UpdateFlags flags) override;
    // 私有渲染诊断：mask 工作输出必须保持为当前二维切片，不得退化为全体积副本。
    std::array<int, 3> GetMaskWorkingDimensions() const;
    // 私有测试诊断：逐像素核对 mask 最近邻采样和最终 masked slice，扫描范围仅为当前二维输出。
    bool GetMaskWorkingPixelsValid() const;
    std::uint64_t GetWorldBoundsBuildCount() const noexcept
    {
        return m_worldBoundsBuildCount;
    }
    std::uint64_t GetInverseBuildCount() const noexcept
    {
        return m_inverseBuildCount;
    }
    int GetNavigationAxis() const override { return (int)m_orientation; }
    // [Public] 业务必需接口：供 Service 查询交互轴向

private:
    class Mapper;
    RenderEffectTarget GetRenderEffectTarget() const override;
    void SetEffectBinding(RenderEffectBinding* binding) override;
    // focusWorld 是世界游标；worldBounds 是模型变换后的世界 AABB。
    // safeOffset 沿当前切片法线偏移两条线，避免十字线与切片共面闪烁。
    void SetCrosshair(const double focusWorld[3],
        const double worldBounds[6],
        double safeOffset);
    void SetWorldBounds(const double bounds[6],
        const std::array<double, 16>& modelMatrix,
        double worldBounds[6]) const; // 把局部数据包围盒映射到当前模型变换后的世界包围盒

    // 非拥有 renderer 弱引用，仅供相机和 clipping range 更新；renderer 销毁后自动为空。
    vtkWeakPointer<vtkRenderer> m_renderer;
    // 切片主 prop 的强引用 owner；同时登记到 m_managedProps，由 renderer 挂载但不转移策略所有权。
    vtkSmartPointer<vtkImageSlice> m_slice;
    // 持有输入 image 和 slice plane 管线连接，按当前平面从体数据抽取二维图像。
    vtkSmartPointer<Mapper> m_mapper;
    // 持久切片平面；SetVisualState 以 world cursor 更新原点，以 orientation 对应主轴更新法线。
    vtkSmartPointer<vtkPlane> m_slicePlane;
    // 最近一次有效输入的强引用和身份缓存；只避免重复绑定，不冻结 vtkImageData 内部内容。
    vtkSmartPointer<vtkDataObject> m_lastInput;
    // 构造期切片轴：Top_down=Z、Front_back=Y、Left_right=X，同时决定相机和十字线配色。
    Orientation m_orientation;

    // --- 十字线相关 ---
    // 两个 actor 由策略强持有并登记到 m_managedProps；线段坐标由对应 source 输出。
    vtkSmartPointer<vtkActor> m_vLineActor;
    vtkSmartPointer<vtkActor> m_hLineActor;
    // 世界坐标十字线几何 producer；Cursor/Transform 更新端点，actor mapper 保持稳定连接。
    vtkSmartPointer<vtkLineSource> m_vLineSource;
    vtkSmartPointer<vtkLineSource> m_hLineSource;
    std::array<double, 6> m_inputBounds{};
    std::array<double, 3> m_inputSpacing{ 1.0, 1.0, 1.0 };
    std::array<double, 6> m_worldBounds{};
    std::array<double, 16> m_modelMatrix{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    vtkMTimeType m_inputGeometryMTime = 0;
    std::uint64_t m_worldBoundsBuildCount = 0;
    std::uint64_t m_inverseBuildCount = 0;
    bool m_hasTransformCache = false;
};
