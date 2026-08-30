#pragma once

#include "App/ViewTypes.h"
#include "Render/Support/FeatureOverlayBase.h"
#include <vtkActor.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLookupTable.h>
#include <vtkPlane.h>
#include <vtkImageProperty.h>

// =====================================================================
// GapMeshOverlayStrategy — 3D孔隙网格叠加策略 (适用于 Volume/IsoSurface 模式)
// =====================================================================
class GapMeshOverlayStrategy : public FeatureOverlayBase {
private:
    // 构造期创建的 3D overlay prop；策略基类与 VTK renderer 共同保留引用。
    vtkSmartPointer<vtkActor> m_actor;
    // actor 使用的 mapper；SetInputData 通过 VTK 引用计数保留最新 void mesh。
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;

public:
    GapMeshOverlayStrategy() {
        m_actor = vtkSmartPointer<vtkActor>::New();
        m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        m_actor->SetMapper(m_mapper);

        m_actor->GetProperty()->SetColor(1.0, 0.0, 0.0); // 红色标出孔隙，和主模型材质色区分开
        m_actor->GetProperty()->SetOpacity(1.0);         // 保持不透明，避免小孔隙在等值面后被背景吞掉
        m_actor->GetProperty()->SetLighting(false);      // 关闭光照，避免红色标签被场景光照改色
        m_actor->SetPickable(false);

		// 多边形偏移设置，防止在极少数重合表面发生 Z-Fighting
        m_mapper->SetResolveCoincidentTopologyToPolygonOffset();

        AttachProp(m_actor);
    }

    // 只接受 vtkPolyData；类型不匹配时保留 mapper 当前输入，调用方清场应走 overlay detach/clear 生命周期。
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {
        auto poly = vtkPolyData::SafeDownCast(data);
        if (poly) {
            m_mapper->SetInputData(poly);
        }
    }

    void SetOverlayState(
        const FeatureOverlayState& state) override {
        // 自动跟随主视图的模型变换（鼠标拖拽旋转平移）。
        Set3DPropsTransform(state.modelToWorld);
    }
};

// =====================================================================
// GapSliceOverlayStrategy — 2D标签图叠加策略 (适用于 Slice 模式)
// =====================================================================
class GapSliceOverlayStrategy : public FeatureOverlayBase {
private:
    // 构造期创建的 2D label prop；策略基类与 VTK renderer 共同保留引用。
    vtkSmartPointer<vtkImageSlice> m_slice;
    // label image reslice mapper；持有最新输入和 SetInputData 创建的 slice plane。
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    // 生命周期覆盖整个策略的标签 LUT：0 透明，所有正标签统一为不透明红色。
    vtkSmartPointer<vtkLookupTable> m_lut;
    // 当前窗口固定轴向；构造后不变，用于选择 plane normal。
    Orientation m_orientation;
public:
    explicit GapSliceOverlayStrategy(Orientation orient) : m_orientation(orient) {
        m_slice = vtkSmartPointer<vtkImageSlice>::New();
        m_mapper = vtkSmartPointer<vtkImageResliceMapper>::New();
        m_slice->SetMapper(m_mapper);

        // 设置红色的透明 LUT 映射
        m_lut = vtkSmartPointer<vtkLookupTable>::New();
        m_lut->SetNumberOfTableValues(256);
        m_lut->SetTableRange(0, 255);
        m_lut->SetTableValue(0, 0.0, 0.0, 0.0, 0.0); // 0 为完全透明
        for (int i = 1; i < 256; ++i) {
            m_lut->SetTableValue(i, 1.0, 0.0, 0.0, 1.0); // 非 0 标签统一显示为红色孔隙
        }
        m_lut->Build(); // 生效

        m_slice->GetProperty()->SetLookupTable(m_lut);
        m_slice->GetProperty()->SetUseLookupTableScalarRange(1);
        m_slice->GetProperty()->SetLayerNumber(1); // 提高层级防止 Z-fighting
		m_slice->GetProperty()->SetInterpolationTypeToNearest(); // 最近邻插值，保持标签边界清晰
        AttachProp(m_slice);
    }

    // 输入 label image 后建立固定轴向切片平面；LUT 把 0 当背景、所有正标签统一显示为红色。
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {
        auto img = vtkImageData::SafeDownCast(data);
        if (!img) return;

        m_mapper->SetInputData(img);
        m_mapper->SliceFacesCameraOff();
        m_mapper->SliceAtFocalPointOff();

        auto plane = vtkSmartPointer<vtkPlane>::New();
        double center[3]; img->GetCenter(center);
        plane->SetOrigin(center);
        if (m_orientation == Orientation::Top_down) plane->SetNormal(0, 0, 1);
        else if (m_orientation == Orientation::Front_back) plane->SetNormal(0, 1, 0);
        else plane->SetNormal(1, 0, 0);
        m_mapper->SetSlicePlane(plane);
    }

    // Transform 同步 overlay 的 modelToWorld；Cursor 把 plane origin 移到当前十字线并沿法线微偏移。
    void SetOverlayState(
        const FeatureOverlayState& state) override {
        // 自动跟随主视图的切片滚动和模型变换。
        auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        modelToWorldMatrix->DeepCopy(state.modelToWorld.data());
        m_slice->SetUserMatrix(modelToWorldMatrix);

        auto plane = m_mapper->GetSlicePlane();
        if (plane) {
            constexpr double sliceOffset = 0.001; // VTK world 坐标偏移，避免标签与原切片共面。
            double worldNormal[3] = { 0.0, 0.0, 0.0 };
            if (m_orientation == Orientation::Top_down) worldNormal[2] = 1.0;
            else if (m_orientation == Orientation::Front_back) worldNormal[1] = 1.0;
            else worldNormal[0] = 1.0;
            double offsetOrigin[3] = {
                state.cursor[0] + worldNormal[0] * sliceOffset,
                state.cursor[1] + worldNormal[1] * sliceOffset,
                state.cursor[2] + worldNormal[2] * sliceOffset
            };

            plane->SetOrigin(offsetOrigin[0], offsetOrigin[1], offsetOrigin[2]);
            plane->SetNormal(worldNormal[0], worldNormal[1], worldNormal[2]);
        }
    }
};
