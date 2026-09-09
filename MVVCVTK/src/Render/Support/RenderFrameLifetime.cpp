#include "Render/Support/RenderFrameLifetime.h"
#include <vtkAbstractArray.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkDataObject.h>
#include <vtkDataSet.h>
#include <vtkFieldData.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkImageMapper3D.h>
#include <vtkImageSlice.h>
#include <vtkMapper.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPointSet.h>
#include <vtkPolyData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkTexture.h>
#include <vtkVolume.h>
#include <vtkVolumeCollection.h>
#include <vtkVolumeMapper.h>
#include <vtkWeakPointer.h>
#ifndef GLAD_API_CALL_EXPORT
#define GLAD_API_CALL_EXPORT
#endif
#include <vtk_glad.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <new>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
struct DrawResources final {
    std::vector<vtkSmartPointer<vtkObject>> values;
    std::unordered_set<vtkObject*> seen;
    std::unordered_set<vtkAlgorithm*> walked;
    bool Add(vtkObject* value) {
        if(!value||seen.find(value)!=seen.end())return false;
        if(seen.size()>=65536)throw std::bad_alloc{};
        seen.insert(value);values.emplace_back(value);return true;
    }
    void Fields(vtkFieldData* fields) {
        if(!fields)return;Add(fields);
        for(int i=0;i<fields->GetNumberOfArrays();++i)Add(fields->GetAbstractArray(i));
    }
    void Data(vtkDataObject* data) {
        if(!data)return;Add(data);Fields(data->GetFieldData());
        if(auto* set=vtkDataSet::SafeDownCast(data)){Fields(set->GetPointData());Fields(set->GetCellData());}
        if(auto* points=vtkPointSet::SafeDownCast(data))if(points->GetPoints()){Add(points->GetPoints());Add(points->GetPoints()->GetData());}
        if(auto* poly=vtkPolyData::SafeDownCast(data))for(auto* cells:{poly->GetVerts(),poly->GetLines(),poly->GetPolys(),poly->GetStrips()})
            if(cells){Add(cells);Add(cells->GetOffsetsArray());Add(cells->GetConnectivityArray());}
    }
    void Pipeline(vtkAlgorithm* algorithm) {
        std::vector<vtkAlgorithm*> pending;if(algorithm)pending.push_back(algorithm);
        while(!pending.empty()) {
            auto* current=pending.back();pending.pop_back();if(!walked.insert(current).second)continue;Add(current);
            for(int port=0;port<current->GetNumberOfOutputPorts();++port)Data(current->GetOutputDataObject(port));
            for(int port=0;port<current->GetNumberOfInputPorts();++port)
                for(int i=0;i<current->GetNumberOfInputConnections(port);++i) {
                    Data(current->GetInputDataObject(port,i));
                    if(auto* input=current->GetInputAlgorithm(port,i))pending.push_back(input);
                }
            if(auto* volume=vtkGPUVolumeRayCastMapper::SafeDownCast(current))Data(volume->GetMaskInput());
        }
    }
    void Renderer(vtkRenderer* renderer) {
        walked.clear();
        auto* props=renderer->GetViewProps();vtkCollectionSimpleIterator cookie;props->InitTraversal(cookie);
        auto actors=vtkSmartPointer<vtkActorCollection>::New();auto volumes=vtkSmartPointer<vtkVolumeCollection>::New();
        while(auto* prop=props->GetNextProp(cookie)) {
            Add(prop);prop->GetActors(actors);prop->GetVolumes(volumes);
            if(auto* slice=vtkImageSlice::SafeDownCast(prop))Pipeline(slice->GetMapper());
        }
        actors->InitTraversal();while(auto* actor=actors->GetNextActor()) {
            Add(actor);Pipeline(actor->GetMapper());if(actor->GetTexture())Pipeline(actor->GetTexture());
        }
        volumes->InitTraversal();while(auto* volume=volumes->GetNextVolume()) {Add(volume);Pipeline(volume->GetMapper());}
    }
};

std::uint64_t CreateFrameId() noexcept {
    static std::atomic<std::uint64_t> next{1};auto value=next.load();
    while(value&&!next.compare_exchange_weak(value,value==std::numeric_limits<std::uint64_t>::max()?0:value+1)){}
    return value;
}
bool SetCurrentContext(vtkOpenGLRenderWindow* window,vtkMTimeType creation) {
    if(!window||window->GetContextCreationTime()!=creation)return false;
    // Qt 托管的窗口没有原生上下文指针，必须通过宿主激活并查询当前上下文。
    window->MakeCurrent();
    return window->IsCurrent()&&window->GetContextCreationTime()==creation;
}
struct FrameFence final : std::enable_shared_from_this<FrameFence> {
    vtkWeakPointer<vtkOpenGLRenderWindow> context;
    vtkMTimeType creation=0;GLsync fence=nullptr;
    std::shared_ptr<DrawResources> resources;
    std::vector<std::function<void(RenderFrameOutcome)>> callbacks;
    RenderFrameOutcome outcome;
    bool complete=false;
    vtkSmartPointer<vtkCallbackCommand> observer;
    unsigned long endTag=0,deleteTag=0;
    ~FrameFence() {
        if(auto* window=context.GetPointer()) {
            window->RemoveObserver(endTag);window->RemoveObserver(deleteTag);
            if(fence&&SetCurrentContext(window,creation))glDeleteSync(fence);
        }
    }
    void Finish(bool succeeded) {
        if(complete)return;complete=true;outcome.isSucceeded=outcome.isSucceeded&&succeeded;
        for(auto& callback:callbacks)try {callback(outcome);}catch(...){}
        callbacks.clear();resources.reset();
    }
    bool Poll(bool deleted=false) {
        if(complete)return false;
        auto* window=context.GetPointer();
        if(deleted||!SetCurrentContext(window,creation)) {
            fence=nullptr;Finish(false);return true;
        }
        const auto result=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,0);
        if(result==GL_TIMEOUT_EXPIRED)return false;
        if(result==GL_WAIT_FAILED)glFinish();
        glDeleteSync(fence);fence=nullptr;Finish(result!=GL_WAIT_FAILED);return true;
    }
    void Observe() {
        auto* window=context.GetPointer();if(!window)return;
        observer=vtkSmartPointer<vtkCallbackCommand>::New();observer->SetClientData(this);
        observer->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
            if(data) {
                // A completion may recursively poll and erase this registry
                // entry while VTK is still dispatching its raw observer.
                auto hold=static_cast<FrameFence*>(data)->shared_from_this();
                (void)hold->Poll(event==vtkCommand::DeleteEvent);
            }
        });
        endTag=window->AddObserver(vtkCommand::EndEvent,observer);
        deleteTag=window->AddObserver(vtkCommand::DeleteEvent,observer);
    }
};
thread_local std::vector<std::shared_ptr<FrameFence>> frames;
thread_local bool isPolling=false;
thread_local std::map<vtkRenderer*,std::weak_ptr<RenderFrameLifetime>> trackers;
}

class RenderFrameLifetime::Impl final {
public:
    explicit Impl(vtkRenderer* value):renderer(value) {
        rendererObserver=vtkSmartPointer<vtkCallbackCommand>::New();rendererObserver->SetClientData(this);
        rendererObserver->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
            if(!data)return;auto& self=*static_cast<Impl*>(data);
            if(event==vtkCommand::StartEvent)self.Begin();
            else {self.Capture();if(self.open&&!self.endTag)self.End();}
        });
        startTag=value->AddObserver(vtkCommand::StartEvent,rendererObserver);
        stopTag=value->AddObserver(vtkCommand::EndEvent,rendererObserver);
    }
    ~Impl() {
        if(renderer){renderer->RemoveObserver(startTag);renderer->RemoveObserver(stopTag);}
        if(window){window->RemoveObserver(endTag);window->RemoveObserver(errorTag);window->RemoveObserver(deleteTag);}
        // Incomplete/aborted preparation still retains issued GPU reads.
        if(open){failed=true;End();}
    }
    void Begin() noexcept {
        try {
            (void)RenderFrameLifetime::PollAll();
            auto* context=renderer?vtkOpenGLRenderWindow::SafeDownCast(renderer->GetRenderWindow()):nullptr;
            if(!context)return;
            if(open&&window.GetPointer()!=context){failed=true;End();}
            context->MakeCurrent();
            if(open){Capture();return;}
            if(window.GetPointer()!=context||!endTag) {
                if(window){window->RemoveObserver(endTag);window->RemoveObserver(errorTag);window->RemoveObserver(deleteTag);}
                endTag=errorTag=deleteTag=0;
                window=context;windowObserver=vtkSmartPointer<vtkCallbackCommand>::New();windowObserver->SetClientData(this);
                windowObserver->SetCallback([](vtkObject*,unsigned long event,void* data,void*) {
                    if(!data)return;auto& self=*static_cast<Impl*>(data);
                    if(event==vtkCommand::ErrorEvent)self.failed=true;
                    else if(event==vtkCommand::DeleteEvent){self.failed=true;self.open=false;self.resources.reset();self.Complete(false);}
                    else self.End();
                });
                endTag=context->AddObserver(vtkCommand::EndEvent,windowObserver);
                errorTag=context->AddObserver(vtkCommand::ErrorEvent,windowObserver);
                deleteTag=context->AddObserver(vtkCommand::DeleteEvent,windowObserver);
            }
            open=true;failed=false;forceFinish=false;frameId=CreateFrameId();
            frameCreation=context->GetContextCreationTime();
            resources=std::make_shared<DrawResources>();Capture();
        } catch(...) {
            // If installing window observers failed, the renderer EndEvent is
            // the synchronous fallback. Leave missing observation retryable.
            if(!open&&window) {
                window->RemoveObserver(endTag);window->RemoveObserver(errorTag);window->RemoveObserver(deleteTag);
                endTag=errorTag=deleteTag=0;
            }
            open=window.GetPointer()!=nullptr;failed=true;forceFinish=true;
            if(window)frameCreation=window->GetContextCreationTime();
        }
    }
    void Capture() noexcept {
        if(!open||!renderer||!resources)return;
        try {resources->Renderer(renderer);}catch(...){failed=true;forceFinish=true;}
    }
    void Complete(bool success) noexcept {
        const RenderFrameOutcome value{frameId,success,window&&window->GetSwapBuffers()!=0};
        auto completions=std::move(callbacks);
        for(auto& callback:completions)try {callback(value);}catch(...){}
    }
    void End() noexcept {
        if(!open)return;Capture();open=false;
        auto owned=std::move(resources);auto completions=std::move(callbacks);
        const bool wasFailed=failed,mustFinish=forceFinish;
        vtkSmartPointer<vtkOpenGLRenderWindow> context=window.GetPointer();
        const auto creation=frameCreation;
        const RenderFrameOutcome result{frameId,!wasFailed,context&&context->GetSwapBuffers()!=0};
        const auto complete=[&](bool success) noexcept {
            auto outcome=result;outcome.isSucceeded=outcome.isSucceeded&&success;
            for(auto& callback:completions)try {callback(outcome);}catch(...){}
            completions.clear();owned.reset();
        };
        if(!SetCurrentContext(context,creation)){complete(false);return;}
        std::shared_ptr<FrameFence> record;
        try {
            // Poll may deliver callbacks and nested frames; this frame is already
            // detached from mutable tracker fields before any callback can run.
            (void)RenderFrameLifetime::PollAll();
            if(!SetCurrentContext(context,creation)){complete(false);return;}
            if(mustFinish||!result.frameId)throw std::bad_alloc{};
            record=std::make_shared<FrameFence>();record->context=context;
            record->creation=creation;record->outcome=result;
            frames.push_back(record);
            record->fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
            if(!record->fence){record->complete=true;throw std::bad_alloc{};}
            record->resources=std::move(owned);record->callbacks=std::move(completions);
            record->Observe();(void)record->Poll();
        } catch(...) {
            if(SetCurrentContext(context,creation)) {
                glFinish();
                if(record&&record->fence){glDeleteSync(record->fence);record->fence=nullptr;}
            }
            if(record)record->Finish(false);
            complete(false);
        }
    }
    bool Queue(std::function<void(RenderFrameOutcome)> callback) {
        if(!open||!callback)return false;
        try {callbacks.push_back(std::move(callback));return true;}catch(...){failed=true;forceFinish=true;return false;}
    }
    vtkWeakPointer<vtkRenderer> renderer;vtkWeakPointer<vtkOpenGLRenderWindow> window;
    vtkSmartPointer<vtkCallbackCommand> rendererObserver,windowObserver;
    unsigned long startTag=0,stopTag=0,endTag=0,errorTag=0,deleteTag=0;
    bool open=false,failed=false,forceFinish=false;std::uint64_t frameId=0;
    vtkMTimeType frameCreation=0;
    std::shared_ptr<DrawResources> resources;
    std::vector<std::function<void(RenderFrameOutcome)>> callbacks;
};

RenderFrameLifetime::RenderFrameLifetime(std::unique_ptr<Impl> impl):m_impl(std::move(impl)){}
RenderFrameLifetime::~RenderFrameLifetime()=default;
std::shared_ptr<RenderFrameLifetime> RenderFrameLifetime::Create(vtkRenderer* renderer) {
    if(!renderer)return {};
    const auto old=trackers.find(renderer);
    if(old!=trackers.end())if(auto alive=old->second.lock())
        if(alive->m_impl->renderer.GetPointer()==renderer)return alive;
    auto result=std::shared_ptr<RenderFrameLifetime>(new RenderFrameLifetime(std::make_unique<Impl>(renderer)));
    trackers[renderer]=result;return result;
}
bool RenderFrameLifetime::PollAll() {
    if(isPolling)return false;
    struct Guard final {Guard(){isPolling=true;}~Guard(){isPolling=false;}} guard;
    bool changed=false;const auto count=frames.size();
    for(std::size_t i=0;i<count;++i){const auto frame=frames[i];changed=frame->Poll()||changed;}
    frames.erase(std::remove_if(frames.begin(),frames.end(),[](const auto& frame){return frame->complete;}),frames.end());
    for(auto it=trackers.begin();it!=trackers.end();)if(it->second.expired())it=trackers.erase(it);else ++it;
    return changed;
}
bool RenderFrameLifetime::QueueCompletion(std::function<void(RenderFrameOutcome)> callback){return m_impl->Queue(std::move(callback));}
bool RenderFrameLifetime::GetHasPending(vtkRenderWindow* window) {
    return window&&std::any_of(frames.begin(),frames.end(),[window](const auto& frame) {
        return !frame->complete&&frame->context.GetPointer()==window;
    });
}
