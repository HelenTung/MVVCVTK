#pragma once
#include "FeatureSupport/SpatialMeshCache.h"
#include "Render/Contracts/FeatureOverlay.h"
#include "Render/Support/RenderTextStyle.h"
#include "Render/Support/SliceContourPlane.h"
#include "Render/Support/SpatialMeshMapper.h"
#include <vtkActor.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellPicker.h>
#include <vtkCompositePolyDataMapper.h>
#include <vtkCutter.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkPlane.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkTextActor.h>
#include <vtkWeakPointer.h>
#include <functional>
#include <climits>
#include <optional>
#include <thread>
#include <cassert>

namespace FeatureInternal {

// 一个视图一个批量 actor；只在相机/模型/切片变化时查 BVH，完成块在 owner 线程挂接。
// 本类没有后台线程，也不认识对象编号；Feature 通过正式面范围设置选择。
class SpatialMeshView final {
public:
    struct Progress final {
        std::size_t requested=0,loaded=0;
        bool isComplete=true,hasFailure=false;
        std::string message;
    };
    SpatialMeshView()
        :m_actor(vtkSmartPointer<vtkActor>::New()),m_mapper(vtkSmartPointer<SpatialMeshMapper>::New()),
        m_blocks(vtkSmartPointer<vtkMultiBlockDataSet>::New()),m_status(vtkSmartPointer<vtkTextActor>::New()),
        m_observer(vtkSmartPointer<vtkCallbackCommand>::New())
    {
        m_actor->SetMapper(m_mapper);m_actor->PickableOff();m_mapper->ScalarVisibilityOff();
        m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
        m_status->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();m_status->SetPosition(.02,.97);
        m_status->GetTextProperty()->SetVerticalJustificationToTop();m_status->GetTextProperty()->SetFontSize(15);
        RenderTextStyle::SetFont(*m_status->GetTextProperty());m_status->VisibilityOff();m_status->PickableOff();
        m_observer->SetClientData(this);
        m_observer->SetCallback([](vtkObject*,unsigned long,void* client,void*){
            auto* view=static_cast<SpatialMeshView*>(client);try{view->Refresh();}catch(...){view->SetFailure("Display view update failed.");}
        });
    }
    ~SpatialMeshView(){DetachRenderer();}
    SpatialMeshView(const SpatialMeshView&)=delete;
    SpatialMeshView& operator=(const SpatialMeshView&)=delete;
    SpatialMeshView(SpatialMeshView&&)=delete;
    SpatialMeshView& operator=(SpatialMeshView&&)=delete;
    vtkActor* GetActor() const noexcept {return m_actor;}
    vtkCompositePolyDataMapper* GetMapper() const noexcept {return m_mapper;}
    vtkTextActor* GetStatusProp() const noexcept {return m_status;}
    Progress GetProgress() const
    {
        auto progress=m_progress;
        if(m_selection){const auto selected=m_selection->GetProgress();progress.requested+=selected.requested;progress.loaded+=selected.loaded;
            progress.isComplete=progress.isComplete&&selected.isComplete;progress.hasFailure=progress.hasFailure||selected.hasFailure;
            if(!selected.message.empty())progress.message=selected.message;}
        return progress;
    }
    const vtkSmartPointer<vtkMultiBlockDataSet>& GetBlocks() const noexcept {return m_blocks;}
    void SetInputData(vtkDataObject* data){m_cache.reset();m_mapper->SetInputDataObject(data);}
    void SetCache(std::shared_ptr<SpatialMeshCache> cache,std::size_t cacheTarget,
        std::function<void()> requestFrame={},std::optional<std::array<double,3>> sliceNormal={})
    {
        if(!cache||!cacheTarget)throw std::invalid_argument("Invalid display cache target.");
        SetObjectRange({});m_firstFace=0;m_endFace=UINT64_MAX;
        m_cache=std::move(cache);m_cacheTarget=cacheTarget;m_requestFrame=std::move(requestFrame);m_sliceNormal=std::move(sliceNormal);
        if(m_sliceNormal)m_cache=std::make_shared<SpatialMeshCache>(m_cache->GetSourceOwner(),m_cache->GetQueue(),m_cacheTarget);
        m_mapper->SetInputDataObject(m_blocks);m_dirty=true;m_planeDirty=true;m_cameraTime=0;m_cacheRevision=0;
        m_blocks->Initialize();m_current.clear();m_styles.clear();m_tickets.clear();m_visible.clear();m_progress={};m_progress.isComplete=false;SetStatus(u8"精确几何准备中");
    }
    void SetRange(std::uint64_t firstFace=0,std::uint64_t endFace=UINT64_MAX)
    {
        if(firstFace!=m_firstFace||endFace!=m_endFace){m_firstFace=firstFace;m_endFace=endFace;m_dirty=true;m_blocks->Initialize();m_current.clear();m_styles.clear();m_tickets.clear();m_visible.clear();}
    }
    void SetState(const FeatureOverlayState& state)
    {
        if(state.modelToWorld!=m_state.modelToWorld||state.cursor!=m_state.cursor){m_dirty=true;m_planeDirty=true;}
        if(state.modelToWorld!=m_state.modelToWorld) {
            vtkNew<vtkMatrix4x4> matrix;matrix->DeepCopy(state.modelToWorld.data());m_actor->SetUserMatrix(matrix);
        }
        m_state=state;
        if(m_selection)m_selection->SetState(state);
    }
    // range 来自所属 Feature 的正式对象目录；这里只管理有限的高亮几何。
    void SetObjectRange(std::optional<std::pair<std::uint64_t,std::uint64_t>> range,bool isolate=false)
    {
        if(!range) {
            if(m_selection){if(m_renderer)m_renderer->RemoveActor(m_selection->GetActor());m_selection->DetachRenderer();m_selection.reset();}
            if(m_isIsolated)SetRange();m_isIsolated=false;return;
        }
        if(!m_cache||range->first>range->second||range->second>m_cache->GetSource().GetFaceCount())throw std::invalid_argument("Object geometry range is unavailable.");
        auto selected=std::make_unique<SpatialMeshView>();
        selected->SetCache(m_cache,m_cacheTarget,m_requestFrame,m_sliceNormal);selected->SetRange(range->first,range->second);selected->SetState(m_state);
        auto* actor=selected->GetActor();actor->GetProperty()->SetColor(1,.75,.1);actor->GetProperty()->LightingOff();
        actor->GetProperty()->SetLineWidth(2.5F);if(!m_sliceNormal)actor->GetProperty()->SetRepresentationToWireframe();
        if(m_renderer){try{m_renderer->AddActor(actor);selected->AttachRenderer(m_renderer);}catch(...){m_renderer->RemoveActor(actor);throw;}}
        if(m_selection){if(m_renderer)m_renderer->RemoveActor(m_selection->GetActor());m_selection->DetachRenderer();}
        m_selection=std::move(selected);SetRange(isolate?range->first:0,isolate?range->second:UINT64_MAX);m_isIsolated=isolate;
    }
    const SpatialMeshView* GetSelection() const noexcept {return m_selection.get();}
    bool GetIsRenderer(const vtkRenderer* renderer) const noexcept {return renderer&&m_renderer.GetPointer()==renderer;}
    void AttachRenderer(vtkRenderer* renderer)
    {
        assert(std::this_thread::get_id()==m_ownerThread);
        if(renderer==m_renderer.GetPointer())return;
        DetachRenderer();m_renderer=renderer;
        if(renderer)m_observerTag=renderer->AddObserver(vtkCommand::StartEvent,m_observer,1.0F);
        if(renderer&&m_selection){renderer->AddActor(m_selection->GetActor());m_selection->AttachRenderer(renderer);}
        m_dirty=true;
    }
    void DetachRenderer() noexcept
    {
        assert(!m_renderer||std::this_thread::get_id()==m_ownerThread);
        if(m_renderer&&m_observerTag)m_renderer->RemoveObserver(m_observerTag);
        if(m_selection){if(m_renderer)m_renderer->RemoveActor(m_selection->GetActor());m_selection->DetachRenderer();}
        m_observerTag=0;m_renderer=nullptr;
    }

    bool Refresh() noexcept
    {
        try {
            bool changed=RefreshImpl();
            if(m_selection){changed=m_selection->Refresh()||changed;const auto selected=m_selection->GetProgress();
                if(m_progress.isComplete){if(selected.hasFailure)SetStatus(u8"所选对象高亮准备失败；正式结果仍可读取");else if(!selected.isComplete)SetStatus(u8"所选对象精确几何准备中");else SetStatus({});}}
            return changed;
        }
        catch(const std::exception& error){try{SetFailure(error.what());}catch(...){m_progress.isComplete=false;m_progress.hasFailure=true;}}
        catch(...){try{SetFailure("Display view update failed.");}catch(...){m_progress.isComplete=false;m_progress.hasFailure=true;}}
        m_failedRefresh=true;m_dirty=false;
        try{if(m_renderer){if(auto* camera=m_renderer->GetActiveCamera())m_cameraTime=camera->GetMTime();m_aspect=m_renderer->GetTiledAspectRatio();}}catch(...){}
        m_cacheRevision=m_cache?m_cache->GetRevision():0;return true;
    }
    bool RefreshImpl()
    {
        if(!m_cache||!m_renderer)return false;
        assert(std::this_thread::get_id()==m_ownerThread);
        bool changed=false;
        auto* camera=m_renderer->GetActiveCamera();if(!camera)return false;
        const auto cameraTime=camera->GetMTime();
        const auto aspect=m_renderer->GetTiledAspectRatio();
        const auto cacheRevision=m_cache->GetRevision();
        const auto before=m_progress;
        if(m_failedRefresh&&!m_dirty&&cameraTime==m_cameraTime&&aspect==m_aspect&&cacheRevision==m_cacheRevision)return false;
        m_failedRefresh=false;
        if(!m_dirty&&cameraTime==m_cameraTime&&aspect==m_aspect&&cacheRevision==m_cacheRevision&&m_progress.isComplete)return false;
        if(m_dirty||cameraTime!=m_cameraTime||aspect!=m_aspect) {
            const bool recut=m_sliceNormal&&m_planeDirty;
            std::optional<std::array<double,4>> section;
            if(m_sliceNormal) {
                vtkNew<vtkPlane> plane;
                if(!SliceContourPlane::SetPlane(*plane,m_state.cursor,*m_sliceNormal,m_state.modelToWorld))throw std::invalid_argument("Display transform is singular.");
                const std::array<double,3> origin{plane->GetOrigin()[0],plane->GetOrigin()[1],plane->GetOrigin()[2]};
                const std::array<double,3> normal{plane->GetNormal()[0],plane->GetNormal()[1],plane->GetNormal()[2]};
                section=std::array<double,4>{normal[0],normal[1],normal[2],-(normal[0]*origin[0]+normal[1]*origin[1]+normal[2]*origin[2])};
                if(m_planeDirty) {
                    m_cache->SetDecoration([origin,normal](vtkPolyData& mesh,const auto&,const auto& cancel){
                        if(cancel())throw std::runtime_error("Slice display cancelled.");
                        vtkNew<vtkPlane> cutPlane;cutPlane->SetOrigin(origin.data());cutPlane->SetNormal(normal.data());
                        vtkNew<vtkCutter> cutter;cutter->SetCutFunction(cutPlane);cutter->GenerateTrianglesOff();cutter->SetInputData(&mesh);cutter->Update();
                        if(cancel())throw std::runtime_error("Slice display cancelled.");
                        mesh.ShallowCopy(cutter->GetOutput());
                    },false);
                }
            }
            double world[24];m_renderer->GetActiveCamera()->GetFrustumPlanes(aspect,world);
            SpatialMeshSource::Planes model{};
            for(unsigned p=0;p<6;++p)for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)
                model[p][col]+=world[4*p+row]*m_state.modelToWorld[4*row+col];
            // 缓存目标只影响回收和并发；完整可见集合始终持续排队，不因总页数拒绝。
            const auto query=m_cache->GetSource().Find(model,m_cache->GetSource().GetBatchCount(),m_firstFace,m_endFace,{},section);
            m_cameraTime=cameraTime;m_aspect=aspect;m_dirty=false;m_planeDirty=false;
            if(!query.isComplete||query.batches.size()>UINT_MAX)throw std::length_error("Display block index representation overflow.");
            if(recut||m_visible!=query.batches) {
                std::vector<vtkSmartPointer<vtkPolyData>> next(query.batches.size());
                std::vector<std::uint64_t> styles(next.size());std::vector<std::shared_ptr<const void>> tickets(next.size());
                if(!recut)for(std::size_t i=0;i<query.batches.size();++i){const auto old=std::lower_bound(m_visible.begin(),m_visible.end(),query.batches[i]);if(old!=m_visible.end()&&*old==query.batches[i]){const auto index=static_cast<std::size_t>(old-m_visible.begin());next[i]=m_current[index];styles[i]=m_styles[index];tickets[i]=m_tickets[index];}}
                m_blocks->SetNumberOfBlocks(static_cast<unsigned>(next.size()));
                for(std::size_t i=0;i<next.size();++i)m_blocks->SetBlock(static_cast<unsigned>(i),next[i]);
                m_visible=query.batches;m_current=std::move(next);m_styles=std::move(styles);m_tickets=std::move(tickets);changed=true;
            }
            m_progress={};m_progress.requested=m_visible.size();
        }
        m_progress.loaded=0;m_progress.hasFailure=false;m_progress.message.clear();
        std::size_t adopted=0,requests=0;const auto style=m_cache->GetStyleVersion();
        for(std::size_t i=0;i<m_visible.size();++i) {
            if(m_current[i]&&m_styles[i]==style){++m_progress.loaded;continue;}
            if(adopted==4||requests==8)continue;
            ++requests;const auto read=m_cache->Get({m_visible[i],m_firstFace,m_endFace},m_current[i]);m_tickets[i]=read.ticket;
            if(!read.error.empty()){m_progress.hasFailure=true;m_progress.message=read.error;}
            if(read.mesh) {
                m_current[i]=read.mesh;m_styles[i]=read.styleVersion;m_tickets[i].reset();m_blocks->SetBlock(static_cast<unsigned>(i),read.mesh);++adopted;++m_progress.loaded;changed=true;
            }
        }
        m_progress.isComplete=m_progress.loaded==m_visible.size()&&!m_progress.hasFailure;
        m_cacheRevision=m_cache->GetRevision();
        if(m_progress.hasFailure)SetStatus(u8"精确几何显示准备失败；正式结果仍可读取");
        else if(m_progress.isComplete)SetStatus({});else SetStatus(u8"当前视域精确几何准备中");
        if(!m_progress.isComplete&&!m_progress.hasFailure&&m_requestFrame)m_requestFrame();
        return changed||before.loaded!=m_progress.loaded||before.isComplete!=m_progress.isComplete
            ||before.hasFailure!=m_progress.hasFailure;
    }
    std::optional<std::uint64_t> GetPickedFace(int x,int y,vtkRenderer* renderer)
    {
        if(!renderer||renderer!=m_renderer.GetPointer()||!m_actor->GetVisibility())return {};
        vtkNew<vtkActor> candidate;candidate->ShallowCopy(m_actor);candidate->PickableOn();
        vtkNew<vtkCellPicker> picker;picker->PickFromListOn();picker->AddPickList(candidate);
        if(!picker->Pick(x,y,0,renderer))return {};
        auto* data=picker->GetDataSet();const auto cell=picker->GetCellId();
        auto* ids=data?vtkIdTypeArray::SafeDownCast(data->GetCellData()->GetArray(SpatialMeshSource::faceIds)):nullptr;
        if(!ids||cell<0||cell>=ids->GetNumberOfValues()||ids->GetValue(cell)<0)return {};
        return static_cast<std::uint64_t>(ids->GetValue(cell));
    }
private:
    void SetStatus(const std::string& text){m_status->SetInput(text.c_str());m_status->SetVisibility(!text.empty());}
    void SetFailure(const std::string& error){m_blocks->Initialize();m_current.clear();m_styles.clear();m_tickets.clear();m_visible.clear();m_progress.isComplete=false;m_progress.hasFailure=true;m_progress.message=error;SetStatus(u8"精确几何显示准备失败；正式结果仍可读取");}
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCompositePolyDataMapper> m_mapper;
    vtkSmartPointer<vtkMultiBlockDataSet> m_blocks;
    vtkSmartPointer<vtkTextActor> m_status;
    vtkSmartPointer<vtkCallbackCommand> m_observer;
    vtkWeakPointer<vtkRenderer> m_renderer;
    unsigned long m_observerTag=0;
    vtkMTimeType m_cameraTime=0;
    double m_aspect=0;
    const std::thread::id m_ownerThread=std::this_thread::get_id();
    std::shared_ptr<SpatialMeshCache> m_cache;
    std::unique_ptr<SpatialMeshView> m_selection;
    std::optional<std::array<double,3>> m_sliceNormal;
    std::function<void()> m_requestFrame;
    FeatureOverlayState m_state;
    std::vector<std::uint64_t> m_visible;
    std::vector<vtkSmartPointer<vtkPolyData>> m_current;
    std::vector<std::uint64_t> m_styles;
    std::vector<std::shared_ptr<const void>> m_tickets;
    std::uint64_t m_cacheRevision=0,m_firstFace=0,m_endFace=UINT64_MAX;
    std::size_t m_cacheTarget=128;
    bool m_dirty=true,m_planeDirty=true,m_failedRefresh=false,m_isIsolated=false;
    Progress m_progress;
};
} // namespace FeatureInternal
