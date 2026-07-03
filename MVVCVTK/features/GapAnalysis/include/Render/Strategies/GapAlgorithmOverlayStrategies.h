#pragma once
#include "BaseVisualStrategy.h"
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
class GapMeshOverlayStrategy : public BaseVisualStrategy {
private:
    vtkSmartPointer<vtkActor> m_actor;
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

        AddManagedProp(m_actor);
    }

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {
        auto poly = vtkPolyData::SafeDownCast(data);
        if (poly) {
            m_mapper->SetInputData(poly);
        }
    }

    void SetVisualState(const RenderParams& params, UpdateFlags flags) override {
        // 自动跟随主视图的模型变换（鼠标拖拽旋转平移）
        if (HasFlag(flags, UpdateFlags::Transform)) {
            Set3DPropsTransform(params.modelMatrix);
        }
    }
};

// =====================================================================
// GapSliceOverlayStrategy — 2D标签图叠加策略 (适用于 Slice 模式)
// =====================================================================
class GapSliceOverlayStrategy : public BaseVisualStrategy {
private:
    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    vtkSmartPointer<vtkLookupTable> m_lut;
    Orientation m_orientation;
    double m_safeOffset[3] = {0}; // 沿法线微量偏移，防止穿模闪烁
public:
    GapSliceOverlayStrategy(Orientation orient) : m_orientation(orient) {
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
        AddManagedProp(m_slice);
    }

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {
        auto img = vtkImageData::SafeDownCast(data);
        if (!img) return;
        img->GetSpacing(m_safeOffset);

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

    void SetVisualState(const RenderParams& params, UpdateFlags flags) override {
        // 自动跟随主视图的切片滚动和模型变换
        if (HasFlag(flags, UpdateFlags::Transform) || HasFlag(flags, UpdateFlags::Cursor)) {
            auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            modelToWorldMatrix->DeepCopy(params.modelMatrix.data());
            m_slice->SetUserMatrix(modelToWorldMatrix);

            auto plane = m_mapper->GetSlicePlane();
            if (plane) {
                double worldNormal[3] = { 0.0, 0.0, 0.0 };
                if (m_orientation == Orientation::Top_down) worldNormal[2] = 1.0;
                else if (m_orientation == Orientation::Front_back) worldNormal[1] = 1.0;
                else worldNormal[0] = 1.0;
				//auto dis = std::max({ m_safeOffset[0], m_safeOffset[1], m_safeOffset[2] });
                double offsetOrigin[3] = {
                    params.cursor[0] + worldNormal[0] * 0.001,
                    params.cursor[1] + worldNormal[1] * 0.001,
                    params.cursor[2] + worldNormal[2] * 0.001
                };

                plane->SetOrigin(offsetOrigin[0], offsetOrigin[1], offsetOrigin[2]);
                plane->SetNormal(worldNormal[0], worldNormal[1], worldNormal[2]);
            }
        }
    }
};
