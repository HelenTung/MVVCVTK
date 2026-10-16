#include "Render/Support/SpatialMeshView.h"
#include <vtkRenderWindow.h>
#include <vtkProperty.h>
#include <vtkCompositePolyDataMapperDelegator.h>
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkOpenGLVertexBufferObjectGroup.h>
#include <vtkOpenGLVertexBufferObject.h>
#include <vtkFieldData.h>
#include <vtkIntArray.h>
#include <vtkUnsignedCharArray.h>
#include <vtkPointData.h>
#include <map>
#include <iostream>
#include <thread>

namespace {
using Source=FeatureInternal::SpatialMeshSource;
using Cache=FeatureInternal::SpatialMeshCache;
// 仅测试观察真实 OpenGL 坐标缓冲上传；使用与生产相同的 composite mapper 路径。
class UploadProbe final:public FeatureInternal::SpatialMeshMapper {
public:
    static UploadProbe* New(){return new UploadProbe;}
    vtkTypeMacro(UploadProbe,FeatureInternal::SpatialMeshMapper);
    std::uint64_t uploadedCoordinateBytes=0,uploadEvents=0;
    bool hasIndependentPageBuffers=true;
    std::array<unsigned char,4> lastColor{};
    unsigned coloredPages=0;
protected:
    void PostRender(const std::vector<vtkSmartPointer<vtkCompositePolyDataMapperDelegator>>& delegates,vtkRenderer*,vtkActor*)override {
        coloredPages=0;
        for(const auto& delegate:delegates){hasIndependentPageBuffers=hasIndependentPageBuffers&&delegate->GetRenderedList().size()==1;
            auto* mapper=vtkOpenGLPolyDataMapper::SafeDownCast(delegate->GetDelegate());
            if(auto* color=mapper?mapper->GetVBOs()->GetVBO("scalarColor"):nullptr){if(color->Download(lastColor.data(),lastColor.size()))++coloredPages;}
            auto* vbo=mapper?mapper->GetVBOs()->GetVBO("vertexMC"):nullptr;if(!vbo)continue;
            const auto time=vbo->GetUploadTime().GetMTime();const auto found=m_uploads.find(vbo->GetHandle());
            if(found==m_uploads.end()||found->second!=time){m_uploads[vbo->GetHandle()]=time;uploadedCoordinateBytes+=vbo->GetSize();++uploadEvents;}
        }
    }
private:std::map<int,vtkMTimeType> m_uploads;
};
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void Work(const std::shared_ptr<FeatureInternal::ReadTaskQueue>& queue)
{
    std::thread worker([queue]{for(;;){std::optional<FeatureInternal::ReadTaskQueue::Job> job;{const std::lock_guard<std::mutex> lock(queue->mutex);job=queue->TakeLocked();}if(!job)break;job->task([token=job->cancelled]{return token->load();});{const std::lock_guard<std::mutex> lock(queue->mutex);queue->FinishLocked();}}});worker.join();
}
std::shared_ptr<const Source> Build(double shift=0,unsigned batches=3)
{
    ResultArrayScope storage;DataArray<double> points;DataArray<std::uint64_t> faces;
    for(unsigned batch=0;batch<batches;++batch){const double x=shift+5*static_cast<double>(batch)-5;for(auto value:{x,0.,0.,x+1,0.,0.,x,1.,0.})points.push_back(value);for(std::size_t face=0;face<Source::facesPerBatch;++face)for(unsigned corner=0;corner<3;++corner)faces.push_back(batch*3+corner);}
    auto mesh=std::make_shared<const SurfaceMeshPayload>(std::move(points),std::move(faces));storage.Finish();return Source::Build(std::move(mesh));
}
void CheckSoftCacheProgress()
{
    constexpr unsigned batches=160;
    const auto queue=std::make_shared<FeatureInternal::ReadTaskQueue>();const auto source=Build(0,batches);
    const auto cache=std::make_shared<Cache>(source,queue,1);
    vtkNew<vtkRenderWindow> window;window->SetSize(1500,500);vtkNew<vtkRenderer> renderer;window->AddRenderer(renderer);
    auto* camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetParallelScale(200);
    camera->SetPosition(395,.5,100);camera->SetFocalPoint(395,.5,0);
    FeatureInternal::SpatialMeshView left,right;left.SetCache(cache,1);right.SetCache(cache,1);left.AttachRenderer(renderer);right.AttachRenderer(renderer);
    const auto finish=[&]{for(int i=0;i<300;++i){left.Refresh();right.Refresh();Work(queue);if(left.GetProgress().isComplete&&right.GetProgress().isComplete)return true;}return false;};
    Require(finish()&&left.GetProgress().requested==batches&&right.GetProgress().loaded==batches,"two views eventually display all pages with a one-page cache target");
    Require(cache->GetCounters().batchesRead==batches,"LRU recycling neither cancels unread completions nor rereads held ready geometry");
    for(int i=0;i<4;++i){left.Refresh();right.Refresh();}
    Require(left.GetProgress().isComplete&&right.GetProgress().isComplete&&cache->GetCounters().batchesRead==batches,"completed views remain complete after cache eviction");
    cache->SetDecoration([](vtkPolyData& mesh,const auto&,const auto& cancel){if(cancel())throw std::runtime_error("cancelled");vtkNew<vtkIntArray> style;style->SetName("test.style");style->InsertNextValue(42);mesh.GetFieldData()->AddArray(style);});
    Require(finish()&&cache->GetCounters().batchesRead==batches,"style updates reuse every held geometry page beyond the cache target");
    for(unsigned i=0;i<batches;++i){auto* block=vtkPolyData::SafeDownCast(left.GetBlocks()->GetBlock(i));auto* style=block?vtkIntArray::SafeDownCast(block->GetFieldData()->GetArray("test.style")):nullptr;Require(style&&style->GetValue(0)==42,"held page carries the current style revision");}
    left.DetachRenderer();right.DetachRenderer();
}
void CheckPendingNotificationReentry()
{
    const auto queue=std::make_shared<FeatureInternal::ReadTaskQueue>();
    const auto cache=std::make_shared<Cache>(Build(),queue);
    unsigned notifications=0;
    bool pendingObserved=false,readyObserved=false;
    queue->onPending=[&] {
        ++notifications;
        const auto reentered=cache->Get({0});
        pendingObserved=reentered.isPending;
        Work(queue);
        readyObserved=cache->Get({0}).mesh!=nullptr;
    };
    const auto requested=cache->Get({0});
    Require(requested.isPending&&notifications==1&&pendingObserved&&readyObserved,
        "notification observes admission and can synchronously consume the queued read");
    Require(cache->Get({0}).mesh!=nullptr&&cache->GetCounters().batchesRead==1,
        "reentry neither resubmits nor loses the prepared page");
}
}
int main(int argc,char** argv)
{
    try {
        if(argc==2&&std::string(argv[1])=="pending-reentry") {CheckPendingNotificationReentry();return 0;}
        CheckPendingNotificationReentry();
        CheckSoftCacheProgress();
        const auto queue=std::make_shared<FeatureInternal::ReadTaskQueue>();
        const auto source=Build();auto cache=std::make_shared<Cache>(source,queue);
        vtkNew<vtkRenderWindow> window;window->SetOffScreenRendering(1);window->SetSize(500,500);
        vtkNew<vtkRenderer> renderer;window->AddRenderer(renderer);
        auto* camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetPosition(0,.5,10);camera->SetFocalPoint(0,.5,0);camera->SetParallelScale(2);
        FeatureInternal::SpatialMeshView view;renderer->AddActor(view.GetActor());renderer->AddViewProp(view.GetStatusProp());view.AttachRenderer(renderer);
        view.SetCache(cache,4);Require(view.Refresh()&&view.GetProgress().requested==1&&!view.GetProgress().isComplete,"view requests one visible spatial batch");
        Work(queue);view.Refresh();Require(view.GetProgress().isComplete&&view.GetBlocks()->GetNumberOfBlocks()==1,"worker-prepared exact geometry becomes visible");
        vtkNew<UploadProbe> uploads;uploads->SetInputDataObject(view.GetBlocks());uploads->ScalarVisibilityOff();view.GetActor()->SetMapper(uploads);
        window->Render();const auto uploaded=uploads->uploadedCoordinateBytes;const auto uploadEvents=uploads->uploadEvents;
        Require(uploaded>0&&uploaded<=3*Source::facesPerBatch*3*sizeof(float),"GPU coordinates contain only the current bounded batch");
        const auto before=cache->GetCounters();const auto dataTime=view.GetBlocks()->GetMTime();auto* central=view.GetBlocks()->GetBlock(0);
        camera->SetPosition(.1,.5,10);camera->SetFocalPoint(.1,.5,0);view.Refresh();
        window->Render();Require(uploads->uploadedCoordinateBytes==uploaded&&uploads->uploadEvents==uploadEvents,"camera motion reuses the actual OpenGL coordinate buffer without uploading it again");
        std::cout<<"GPU coordinate uploads bytes="<<uploaded<<" events="<<uploadEvents<<" cameraAdditionalBytes="<<uploads->uploadedCoordinateBytes-uploaded<<'\n';
        Require(cache->GetCounters().batchesRead==before.batchesRead&&view.GetBlocks()->GetBlock(0)==central&&view.GetBlocks()->GetMTime()==dataTime,"camera motion inside the same batch changes no geometry or mapper input time");
        const auto cameraTime=camera->GetMTime();window->SetSize(1500,500);view.Refresh();
        Require(camera->GetMTime()==cameraTime&&view.GetProgress().requested==3,"viewport aspect changes invalidate the frustum without a camera change");
        Work(queue);view.Refresh();Require(view.GetProgress().isComplete&&cache->GetCounters().batchesRead==3,"wider view reads only newly visible batches");
        window->Render();Require(uploads->hasIndependentPageBuffers&&uploads->uploadEvents==3&&uploads->uploadedCoordinateBytes==3*uploaded,
            "new spatial pages upload only their own VBOs without repacking already visible geometry");
        cache->SetDecoration([](vtkPolyData& mesh,const auto&,const auto& cancel){if(cancel())throw std::runtime_error("cancelled");
            vtkNew<vtkIntArray> style;style->SetName("style-only");style->InsertNextValue(1);mesh.GetFieldData()->AddArray(style);});
        for(int i=0;i<8;++i){view.Refresh();Work(queue);if(view.GetProgress().isComplete)break;}
        window->Render();Require(view.GetProgress().isComplete&&uploads->uploadedCoordinateBytes==3*uploaded&&uploads->uploadEvents==3,
            "style wrappers reuse the same actual coordinate VBOs across delegate identities");
        uploads->SetScalarModeToUsePointData();uploads->SetColorModeToDirectScalars();uploads->ScalarVisibilityOn();
        for(const bool green:{false,true}) {
            cache->SetDecoration([green](vtkPolyData& mesh,const auto&,const auto&){vtkNew<vtkUnsignedCharArray> color;color->SetNumberOfComponents(3);color->SetNumberOfTuples(mesh.GetNumberOfPoints());
                for(vtkIdType i=0;i<mesh.GetNumberOfPoints();++i)color->SetTuple3(i,green?0:255,green?255:0,0);mesh.GetPointData()->SetScalars(color);});
            for(int i=0;i<8;++i){view.Refresh();Work(queue);if(view.GetProgress().isComplete)break;}
            window->Render();Require(uploads->coloredPages==3&&uploads->lastColor[0]==(green?0:255)&&uploads->lastColor[1]==(green?255:0)
                &&uploads->uploadedCoordinateBytes==3*uploaded&&uploads->uploadEvents==3,"style changes upload the new GPU colors while preserving every coordinate VBO");
        }
        view.GetActor()->SetMapper(view.GetMapper());
        view.SetCache(cache,1);view.Refresh();Require(view.GetProgress().isComplete&&view.GetBlocks()->GetNumberOfBlocks()==3,"a small cache target cannot reject the complete visible result");
        view.SetRange(Source::facesPerBatch+10,Source::facesPerBatch+13);
        Require(view.GetBlocks()->GetNumberOfBlocks()==0,"changing object range clears the previous selection immediately");
        view.Refresh();Work(queue);view.Refresh();auto* selected=vtkPolyData::SafeDownCast(view.GetBlocks()->GetBlock(0));
        Require(selected&&selected->GetNumberOfCells()==3,"selection reads only the object's requested face range");
        view.SetRange();view.Refresh();
        const auto formalFaces=source->GetFaceCount();const auto readBeforeSelection=cache->GetCounters().facesRead;
        view.SetObjectRange(std::make_pair(Source::facesPerBatch+10,Source::facesPerBatch+13));view.Refresh();Work(queue);view.Refresh();
        Require(view.GetSelection()&&view.GetSelection()->GetBlocks()->GetNumberOfBlocks()==1
            &&vtkPolyData::SafeDownCast(view.GetSelection()->GetBlocks()->GetBlock(0))->GetNumberOfCells()==3
            &&cache->GetCounters().facesRead==readBeforeSelection,"highlight reuses a bounded object range without rebuilding or rereading the full mesh");
        view.SetObjectRange(std::make_pair(Source::facesPerBatch+20,Source::facesPerBatch+22));view.Refresh();
        Require(view.GetSelection()->GetBlocks()->GetNumberOfBlocks()==0||!view.GetSelection()->GetBlocks()->GetBlock(0),"new object selection removes the prior highlight before async preparation");Work(queue);view.Refresh();
        const auto selectedReadCount=cache->GetCounters().facesRead;
        view.SetObjectRange(std::make_pair(Source::facesPerBatch+20,Source::facesPerBatch+22),true);view.Refresh();Work(queue);view.Refresh();
        Require(view.GetBlocks()->GetNumberOfBlocks()==1&&cache->GetCounters().facesRead==selectedReadCount
            &&source->GetFaceCount()==formalFaces,"isolation changes only current display ranges and retains all formal faces");
        view.SetObjectRange({});view.Refresh();Require(!view.GetSelection()&&view.GetBlocks()->GetNumberOfBlocks()==3,"clearing selection restores exact visible batches");
        FeatureOverlayState transformed;transformed.modelToWorld={0,-2,0,4,3,0,0,-2,0,0,.5,3,0,0,0,1};view.SetState(transformed);
        camera->SetPosition(3,-.5,13);camera->SetFocalPoint(3,-.5,3);window->SetSize(500,500);view.Refresh();Work(queue);view.Refresh();
        Require(view.GetProgress().requested==1&&view.GetActor()->GetUserMatrix()->GetElement(0,3)==4,"query and actor use the same non-unit model transform");
        renderer->SetWorldPoint(3.5,-1.25,3,1);renderer->WorldToDisplay();double display[3];renderer->GetDisplayPoint(display);
        const auto picked=view.GetPickedFace(static_cast<int>(display[0]),static_cast<int>(display[1]),renderer);
        Require(picked&&*picked>=Source::facesPerBatch&&*picked<2*Source::facesPerBatch,"actor-scoped composite picker restores exact global face identity");
        const auto other=std::make_shared<Cache>(Build(100),queue);view.SetCache(other,4);view.SetState({});view.Refresh();
        Require(view.GetBlocks()->GetNumberOfBlocks()==0&&view.GetProgress().isComplete,"empty visibility after result replacement retains no old blocks");
        window->Render(); // 清空输入后的 mapper 更新也不能解引用上一修订的 page。
        view.SetCache(cache,4,{},std::array<double,3>{1,0,0});FeatureOverlayState slice;slice.cursor={.5,0,0};view.SetState(slice);
        camera->SetPosition(0,.5,10);camera->SetFocalPoint(0,.5,0);view.Refresh();Work(queue);view.Refresh();
        auto* contour=vtkPolyData::SafeDownCast(view.GetBlocks()->GetBlock(0));
        Require(view.GetProgress().isComplete&&view.GetProgress().requested==1&&contour&&contour->GetNumberOfLines()==Source::facesPerBatch,"slice worker reads and cuts only plane-intersecting spatial batches");
        auto* ids=vtkIdTypeArray::SafeDownCast(contour->GetCellData()->GetArray(Source::faceIds));
        Require(ids&&ids->GetValue(0)>=Source::facesPerBatch&&ids->GetValue(ids->GetNumberOfValues()-1)<2*Source::facesPerBatch,"slice cutting preserves global formal face IDs");
        slice.cursor={5.5,0,0};view.SetState(slice);view.Refresh();Require(view.GetProgress().loaded==0&&view.GetBlocks()->GetNumberOfBlocks()==0,"new slice outside the current frustum cannot show contours from the old plane");
        camera->SetPosition(5.5,.5,10);camera->SetFocalPoint(5.5,.5,0);view.Refresh();Work(queue);view.Refresh();
        contour=vtkPolyData::SafeDownCast(view.GetBlocks()->GetBlock(0));Require(contour&&contour->GetBounds()[0]>5,"new slice is delivered from the same formal result");
        FeatureOverlayState singular;singular.modelToWorld.fill(0);view.SetState(singular);view.Refresh();
        Require(view.GetProgress().hasFailure&&!view.GetProgress().isComplete&&source->GetMesh().GetValid(),"display transform failure stays inside the display boundary and preserves formal data");
        Require(!view.Refresh(),"unchanged failed display does not continuously resubmit or redraw");
        view.DetachRenderer();renderer->RemoveActor(view.GetActor());renderer->RemoveViewProp(view.GetStatusProp());
        std::cout<<"Spatial view: camera reuse, viewport resize, exact capacity, scoped picking, slice worker and replacement passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
