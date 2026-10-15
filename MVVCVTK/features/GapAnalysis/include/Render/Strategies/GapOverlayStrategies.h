#pragma once

#include "Render/Contracts/SlicePlaneState.h"
#include "App/ViewTypes.h"
#include "Render/Support/FeatureOverlayBase.h"
#include "Render/Support/AnalysisLegend.h"
#include "Render/GapDisplayData.h"
#include "Render/Support/SliceContourPlane.h"
#include "Render/Support/SpatialMeshView.h"
#include <vtkCutter.h>
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
    FeatureInternal::SpatialMeshView m_stream;
    // 构造期创建的 3D overlay prop；策略基类与 VTK renderer 共同保留引用。
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<AnalysisLegend> m_legend;
    // actor 使用的 mapper；SetInputData 通过 VTK 引用计数保留最新 void mesh。
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;

public:
    explicit GapMeshOverlayStrategy(std::shared_ptr<const GapDisplayData> display = {}) {
        m_actor = m_stream.GetActor();
        m_mapper = m_stream.GetMapper();
        m_actor->SetMapper(m_mapper);

        m_actor->GetProperty()->SetColor(1.0, 0.0, 0.0); // 红色标出孔隙，和主模型材质色区分开
        m_actor->GetProperty()->SetOpacity(1.0);         // 保持不透明，避免小孔隙在等值面后被背景吞掉
        m_actor->GetProperty()->SetLighting(false);      // 关闭光照，避免红色标签被场景光照改色
        m_actor->SetPickable(false);
        AttachProp(m_actor);
        if (display) {
            m_mapper->ScalarVisibilityOn();
            m_mapper->SetScalarModeToUseCellData();
            m_mapper->SetLookupTable(display->labels);
            m_mapper->UseLookupTableScalarRangeOn();
            m_actor->GetProperty()->SetOpacity(display->params.opacity);
            m_legend=vtkSmartPointer<AnalysisLegend>::New();auto& legend=m_legend;
            legend->SetLookupTable(display->volumes);
            AnalysisColorStyle::SetLegend(*legend, u8"孔隙体积 [mm³]");
            legend->SetMapping(display->mappingRange,GapDisplayData::GetColorRamp(display->params),display->hasRegions);
            legend->SetPosition(display->params.legendPosition[0],display->params.legendPosition[1]);
            legend->SetVisibility(display->params.hasLegend);
            AttachProp(legend);
        }

		// 多边形偏移设置，防止在极少数重合表面发生 Z-Fighting
        m_mapper->SetResolveCoincidentTopologyToPolygonOffset();

    }

    // 只接受 vtkPolyData；类型不匹配时保留 mapper 当前输入，调用方清场应走 overlay detach/clear 生命周期。
    void SetPresentation(const GapDisplayParams& params) {
        m_actor->GetProperty()->SetOpacity(params.opacity);if(m_legend){m_legend->SetVisibility(params.hasLegend);m_legend->SetPosition(params.legendPosition[0],params.legendPosition[1]);}
    }
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
        m_stream.SetState(state);
    }
    void SetDisplayCache(std::shared_ptr<FeatureInternal::SpatialMeshCache> cache,std::size_t capacity,std::function<void()> notify) {
        m_stream.SetCache(std::move(cache),capacity,std::move(notify));AttachProp(m_stream.GetStatusProp());
    }
    FeatureInternal::SpatialMeshView& GetDisplay() noexcept {return m_stream;}
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {FeatureOverlayBase::AttachRenderer(renderer);m_stream.AttachRenderer(renderer);}
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {m_stream.DetachRenderer();FeatureOverlayBase::DetachRenderer(renderer);}
};

// =====================================================================
// GapSliceOverlayStrategy — 2D标签图叠加策略 (适用于 Slice 模式)
// =====================================================================
class GapSliceOverlayStrategy : public FeatureOverlayBase {
private:
    FeatureInternal::SpatialMeshView m_stream;
    // 构造期创建的 2D label prop；策略基类与 VTK renderer 共同保留引用。
    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<AnalysisLegend> m_legend;
    // label image reslice mapper；持有最新输入和 SetInputData 创建的 slice plane。
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    // 生命周期覆盖整个策略的标签 LUT：0 透明，所有正标签统一为不透明红色。
    vtkSmartPointer<vtkLookupTable> m_lut;
    vtkSmartPointer<vtkLookupTable> m_baseLut;
    std::optional<std::int32_t> m_onlyRegion;
    // 当前窗口固定轴向；构造后不变，用于选择 plane normal。
    Orientation m_orientation;
    vtkSmartPointer<vtkCutter> m_contour;
    vtkSmartPointer<vtkActor> m_contourActor;
    vtkSmartPointer<vtkPlane> m_contourPlane=vtkSmartPointer<vtkPlane>::New();
public:
    explicit GapSliceOverlayStrategy(Orientation orient,
        std::shared_ptr<const GapDisplayData> display = {}) : m_orientation(orient) {
        m_slice = vtkSmartPointer<vtkImageSlice>::New();
        m_mapper = vtkSmartPointer<vtkImageResliceMapper>::New();
        m_slice->SetMapper(m_mapper);
        AttachProp(m_slice);

        // 设置红色的透明 LUT 映射
        m_lut = vtkSmartPointer<vtkLookupTable>::New();
        m_lut->SetNumberOfTableValues(256);
        m_lut->SetTableRange(0, 255);
        m_lut->SetTableValue(0, 0.0, 0.0, 0.0, 0.0); // 0 为完全透明
        for (int i = 1; i < 256; ++i) {
            m_lut->SetTableValue(i, 1.0, 0.0, 0.0, 1.0); // 非 0 标签统一显示为红色孔隙
        }
        m_lut->Build(); // 生效
        if (display) {
            m_lut = display->labels;
            m_slice->GetProperty()->SetOpacity(0.35*display->params.opacity);
            m_legend=vtkSmartPointer<AnalysisLegend>::New();auto& legend=m_legend;
            legend->SetLookupTable(display->volumes);
            AnalysisColorStyle::SetLegend(*legend, u8"孔隙体积 [mm³]");
            legend->SetMapping(display->mappingRange,GapDisplayData::GetColorRamp(display->params),display->hasRegions);
            legend->SetPosition(display->params.legendPosition[0],display->params.legendPosition[1]);
            legend->SetVisibility(display->params.hasLegend);
            AttachProp(legend);
            m_contour = vtkSmartPointer<vtkCutter>::New();
            m_contour->SetInputData(display->mesh);
            m_contour->GenerateTrianglesOff();
            auto contourMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            contourMapper->SetInputConnection(m_contour->GetOutputPort());
            contourMapper->ScalarVisibilityOff();
            m_contourActor = m_stream.GetActor();
            m_contourActor->SetMapper(contourMapper);
            m_contourActor->GetProperty()->SetColor(1, 1, 1);
            m_contourActor->GetProperty()->LightingOff();
            m_contourActor->GetProperty()->SetLineWidth(1.5);
            m_contourActor->PickableOff();
            AttachProp(m_contourActor);
        }

        m_baseLut=m_lut;
        m_slice->GetProperty()->SetLookupTable(m_lut);
        m_slice->GetProperty()->SetUseLookupTableScalarRange(1);
        m_slice->GetProperty()->SetLayerNumber(1); // 提高层级防止 Z-fighting
		m_slice->GetProperty()->SetInterpolationTypeToNearest(); // 最近邻插值，保持标签边界清晰
    }

    // 输入 label image 后建立固定轴向切片平面；LUT 把 0 当背景、所有正标签统一显示为红色。
    void SetPresentation(const GapDisplayParams& params) {
        m_slice->GetProperty()->SetOpacity(.35*params.opacity);if(m_legend){m_legend->SetVisibility(params.hasLegend);m_legend->SetPosition(params.legendPosition[0],params.legendPosition[1]);}
    }
    void SetDisplayCache(std::shared_ptr<FeatureInternal::SpatialMeshCache> cache,std::size_t capacity,std::function<void()> notify) {
        m_contour=nullptr;
        m_stream.SetCache(std::move(cache),capacity,std::move(notify),SlicePlaneState::Build(m_orientation,{}).worldNormal);
        m_contourActor->SetMapper(m_stream.GetMapper());AttachProp(m_stream.GetStatusProp());
    }
    FeatureInternal::SpatialMeshView& GetDisplay() noexcept {return m_stream;}
    void SetObjectSelection(std::optional<std::pair<std::uint64_t,std::uint64_t>> range,std::optional<std::int32_t> id,bool isolated) {
        vtkSmartPointer<vtkLookupTable> labels=m_baseLut;
        if(isolated&&id){auto selected=vtkSmartPointer<GapLabelLookupTable>::New();selected->DeepCopy(m_baseLut);selected->SetOnlyRegion(id);labels=selected;}
        m_stream.SetObjectRange(range,isolated);m_onlyRegion=isolated?id:std::nullopt;m_lut=labels;m_slice->GetProperty()->SetLookupTable(labels);
    }
    std::optional<std::int32_t> GetPickedRegion(int x,int y,vtkRenderer* renderer) {
        if(!m_stream.GetIsRenderer(renderer)||!m_slice->GetVisibility())return {};
        auto* image=m_mapper->GetInput();auto* values=image?vtkIntArray::SafeDownCast(image->GetPointData()->GetScalars()):nullptr;
        if(!values)return {};
        vtkNew<vtkImageSlice> candidate;candidate->ShallowCopy(m_slice);candidate->PickableOn();
        vtkNew<vtkCellPicker> picker;picker->PickFromListOn();picker->AddPickList(candidate);
        if(!picker->Pick(x,y,0,renderer))return {};
        const auto point=picker->GetPointId();if(point<0||point>=values->GetNumberOfValues())return {};
        const auto id=values->GetValue(point);return id>0&&(!m_onlyRegion||id==*m_onlyRegion)&&m_lut->GetOpacity(id)>0?std::optional<std::int32_t>{id}:std::nullopt;
    }
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {FeatureOverlayBase::AttachRenderer(renderer);m_stream.AttachRenderer(renderer);}
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {m_stream.DetachRenderer();FeatureOverlayBase::DetachRenderer(renderer);}
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {
        auto img = vtkImageData::SafeDownCast(data);
        if (!img) return;

        m_mapper->SetInputData(img);
        m_mapper->SliceFacesCameraOff();
        m_mapper->SliceAtFocalPointOff();

        auto plane = vtkSmartPointer<vtkPlane>::New();
        double center[3]; img->GetCenter(center);
        plane->SetOrigin(center);
        const auto planeState = SlicePlaneState::Build(m_orientation, {});
        plane->SetNormal(planeState.worldNormal.data());
        m_mapper->SetSlicePlane(plane);
        if (m_contour) m_contour->SetCutFunction(m_contourPlane);
    }

    // Transform 同步 overlay 的 modelToWorld；Cursor 把 plane origin 移到当前十字线并沿法线微偏移。
    void SetOverlayState(
        const FeatureOverlayState& state) override {
        // 自动跟随主视图的切片滚动和模型变换。
        Set3DPropsTransform(state.modelToWorld);

        auto plane = m_mapper->GetSlicePlane();
        if (plane) {
            constexpr double sliceOffset = 0.001; // VTK world 坐标偏移，避免标签与原切片共面。
            const auto planeState = SlicePlaneState::Build(m_orientation, state.cursor, sliceOffset);
            plane->SetOrigin(planeState.worldOrigin.data());
            plane->SetNormal(planeState.worldNormal.data());
            if (m_contour) (void)SliceContourPlane::SetPlane(*m_contourPlane,
                planeState.worldOrigin,planeState.worldNormal,state.modelToWorld);
            auto contourState=state;contourState.cursor=planeState.worldOrigin;m_stream.SetState(contourState);
        }
    }
};
