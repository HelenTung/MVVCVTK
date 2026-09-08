#include "CropBridgeTests.h"
#include "../TestDataPort.h"

#include "Algorithms/CropAlgorithm.h"
#include "Interaction/CropBridge.h"
#include "Render/CropShaderController.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Support/RenderFrameLifetime.h"

#include <vtkCubeSource.h>
#include <vtkImageData.h>
#include <vtkCommand.h>
#include <vtkCallbackCommand.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
CropInputSnapshot BuildGraphInput(
    TestDataPort& data,
    vtkImageData* image,
    const CropBoundsDouble6Array& bounds)
{
    const auto view = data.SetPrimaryImage(image);
    CropInputSnapshot input;
    if (!view) return input;
    input.graph = view->graph;
    input.binding = view->binding;
    input.data = view->data;
    input.inputModelBounds = bounds;
    input.image = view;
    return input;
}

class CropServiceStub final : public FeatureViewService {
public:
    CropServiceStub(
        const RenderInputStamp inputStamp,
        vtkSmartPointer<vtkRenderer> renderer)
        : m_inputStamp(inputStamp)
        , m_renderer(std::move(renderer))
        , m_strategy(std::make_shared<IsoSurfaceStrategy>())
    {
        auto cube = vtkSmartPointer<vtkCubeSource>::New();
        cube->Update();
        m_strategy->SetInputData(cube->GetOutput());
        (void)m_strategy->SetRenderInputStamp(m_inputStamp);
        m_strategy->AttachRenderer(m_renderer);
    }

    ~CropServiceStub() override
    {
        if (m_strategy && m_renderer) {
            m_strategy->DetachRenderer(m_renderer);
        }
    }

    bool SetInteracting(
        const InteractionSource& source,
        const bool isInteracting) override
    {
        const auto sourceIt = std::find(
            m_sources.begin(), m_sources.end(), source);
        if (isInteracting && sourceIt == m_sources.end()) {
            m_sources.push_back(source);
        }
        else if (!isInteracting && sourceIt != m_sources.end()) {
            m_sources.erase(sourceIt);
        }
        return true;
    }
    bool GetIsInteracting() const { return !m_sources.empty(); }
    std::optional<std::array<double, 16>>
        GetModelToWorld() const override
    {
        return std::array<double, 16>{
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0 };
    }
    std::optional<std::array<double, 3>> GetWorldPosition(
        const std::array<double, 3>& modelPosition) const override
    {
        return modelPosition;
    }
    std::optional<RenderInputStamp>
        GetRenderInputStamp() const override
    {
        return m_inputStamp;
    }

    bool AttachRenderEffect(std::shared_ptr<RenderEffect> effect) override
    {
        if (!effect || !m_effect.expired() || !m_strategy) {
            return false;
        }
        if (!m_strategy->SetRenderInputStamp(m_inputStamp)
            || !m_strategy->AttachRenderEffect(
                effect, RenderBindingUse::Current)) {
            return false;
        }
        m_effect = std::dynamic_pointer_cast<CropShaderEffect>(effect);
        ++attachCount;
        return !m_effect.expired();
    }

    bool DetachRenderEffect(const RenderEffect* effect) override
    {
        auto current = m_effect.lock();
        if (!current || current.get() != effect || !m_strategy
            || !m_strategy->DetachRenderEffect(effect)) {
            return false;
        }
        m_effect.reset();
        ++detachCount;
        return true;
    }

    bool SetRenderInputStamp(const RenderInputStamp inputStamp)
    {
        m_inputStamp = inputStamp;
        return m_strategy
            && m_strategy->SetRenderInputStamp(inputStamp);
    }

    RenderEffectState GetEffectState() const
    {
        const auto effect = m_effect.lock();
        return effect ? effect->GetState() : RenderEffectState{};
    }
    bool GetPointVisible(const std::array<double,3>& point) const {
        const auto effect=m_effect.lock();return !effect||effect->GetPointVisible(m_inputStamp,point);
    }
    bool SetRenderNeeded() override
    {
        ++dirtyCount;
        return true;
    }

    bool StartCandidate(RenderInputStamp stamp) {
        const auto effect=m_effect.lock();if(!effect||m_candidate)return false;
        auto cube=vtkSmartPointer<vtkCubeSource>::New();cube->Update();
        auto candidate=std::make_shared<IsoSurfaceStrategy>();
        candidate->SetInputData(cube->GetOutput());
        if(!candidate->SetRenderInputStamp(stamp)
            ||!candidate->AttachRenderEffect(effect,RenderBindingUse::Candidate))return false;
        candidate->AttachRenderer(m_renderer);m_candidate=std::move(candidate);return true;
    }
    RenderEffectState GetCandidateState() const {return m_candidate?m_candidate->GetRenderEffectState():RenderEffectState{};}
    RenderEffectState GetCurrentState() const {return m_strategy->GetRenderEffectState();}
    bool SetCandidateView() {
        if(!m_candidate||m_candidate->GetRenderEffectState().status!=RenderEffectStatus::Committed
            ||!m_candidate->SetRenderEffectUse(RenderBindingUse::Current))return false;
        m_strategy->DetachRenderer(m_renderer);m_retiring=std::move(m_strategy);m_strategy=std::move(m_candidate);
        m_inputStamp=m_strategy->GetRenderInputStamp();return true;
    }
    void ClearCandidate() {if(m_candidate)m_candidate->DetachRenderer(m_renderer);m_candidate.reset();}
    void CompleteCandidate() {m_retiring.reset();}

    int attachCount = 0;
    int detachCount = 0;
    int dirtyCount = 0;

private:
    RenderInputStamp m_inputStamp;
    vtkSmartPointer<vtkRenderer> m_renderer;
    std::shared_ptr<IsoSurfaceStrategy> m_strategy;
    std::shared_ptr<IsoSurfaceStrategy> m_candidate,m_retiring;
    std::weak_ptr<CropShaderEffect> m_effect;
    std::vector<InteractionSource> m_sources;
};

bool SendWidgetInput(
    vtkRenderer* renderer,
    vtkRenderWindowInteractor* interactor,
    const int moveOffset = 8,
    const bool isReleased = true,
    const bool isMoveSent = true)
{
    if (!renderer || !interactor) {
        return false;
    }
    renderer->SetWorldPoint(3.0, 1.5, 1.5, 1.0);
    renderer->WorldToDisplay();
    const auto* displayPoint = renderer->GetDisplayPoint();
    const int x = static_cast<int>(displayPoint[0]);
    const int y = static_cast<int>(displayPoint[1]);
    interactor->SetEventPosition(x, y);
    interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    if (isMoveSent) {
        interactor->SetEventPosition(x + moveOffset, y);
        interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    }
    if (isReleased) {
        interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    }
    return true;
}

bool SendPlaneInput(
    vtkRenderer* renderer,
    vtkRenderWindowInteractor* interactor,
    const int moveOffset)
{
    if (!renderer || !interactor) {
        return false;
    }
    renderer->SetWorldPoint(1.5, 1.5, 1.5, 1.0);
    renderer->WorldToDisplay();
    const auto* displayPoint = renderer->GetDisplayPoint();
    const int x = static_cast<int>(displayPoint[0]);
    const int y = static_cast<int>(displayPoint[1]);
    interactor->SetEventPosition(x, y);
    interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
    interactor->SetEventPosition(x + moveOffset, y);
    interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
    interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    return true;
}

bool SendShaderCommit(
    CropBridge& bridge,
    vtkRenderWindow* renderWindow)
{
    if (!renderWindow) {
        return false;
    }
    renderWindow->Render();
    return bridge.SendShaderCommit();
}
}

namespace {
struct Fixture final {
    TestDataPort data;
    vtkSmartPointer<vtkImageData> image=vtkSmartPointer<vtkImageData>::New();
    vtkSmartPointer<vtkRenderer> renderer=vtkSmartPointer<vtkRenderer>::New();
    vtkSmartPointer<vtkRenderWindow> window=vtkSmartPointer<vtkRenderWindow>::New();
    vtkSmartPointer<vtkRenderWindowInteractor> interactor=vtkSmartPointer<vtkRenderWindowInteractor>::New();
    std::shared_ptr<FeatureViewLease> lease=std::make_shared<FeatureViewLease>(std::this_thread::get_id());
    CropInputSnapshot input;
    std::shared_ptr<CropServiceStub> service;
    CropViewRequest view;
    CropBridge bridge;
    bool ready=false;
    Fixture() {
        image->SetDimensions(4,4,4);image->AllocateScalars(VTK_FLOAT,1);
        std::fill_n(static_cast<float*>(image->GetScalarPointer()),64,1.0f);
        input=BuildGraphInput(data,image,{0,3,0,3,0,3});
        window->SetOffScreenRendering(1);window->SetSize(200,200);window->AddRenderer(renderer);
        interactor->SetRenderWindow(window);
        service=std::make_shared<CropServiceStub>(RenderInputStamp{input.data->self},renderer);
        view={interactor,renderer,lease,service,{service}};
        ready=bridge.StartView(view)&&bridge.SetCropInput(input);
        renderer->ResetCamera(input.inputModelBounds.data());window->Render();
    }
    ~Fixture() {bridge.CancelPending();bridge.ClearBindings();service.reset();window->Finalize();}
};
bool Check(bool value,const char* message) {if(!value)std::cerr<<"Bridge: "<<message<<'\n';return value;}
bool Flush(CropBridge& bridge,vtkRenderWindow* window) {
    for(int frame=0;frame<40&&bridge.GetShaderTickNeeded();++frame) {window->Render();bridge.SendShaderCommit();}
    return !bridge.GetShaderTickNeeded();
}
bool Deliver(CropBridge& bridge) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!bridge.GetBuildTickNeeded()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return bridge.SendBuildResult();
}
CropEditRequest Request(CropBridge& bridge,CropEditKind kind,CropNodeId node) {
    const auto history=bridge.GetHistory();CropEditRequest request;
    request.documentId=history.documentId;request.requestId=CropHistory::CreateNodeId();
    request.expectedRevision=history.stateRevision;request.kind=kind;request.nodeId=node;
    request.operation.boxToInputModelMatrix={3,0,0,1.5,0,3,0,1.5,0,0,3,1.5,0,0,0,1};
    return request;
}
CropEditAdmission Append(CropBridge& bridge,CropNodeId parent) {return bridge.SendRequest(Request(bridge,CropEditKind::Append,parent));}

bool GetWidgetAndSiblingEdits() {
    Fixture f;if(!f.ready||!f.bridge.SwitchCropBox())return false;
    if(!Check(SendWidgetInput(f.renderer,f.interactor)&&!f.bridge.GetShaderTickNeeded()
        &&f.bridge.GetHistory().totalNodeCount==1,"None release created a node"))return false;
    if(!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    if(!Check(SendWidgetInput(f.renderer,f.interactor,0)&&!f.bridge.GetShaderTickNeeded()
        &&!f.service->GetIsInteracting(),"zero-distance box drag created history or retained interaction"))return false;
    if(!SendWidgetInput(f.renderer,f.interactor))return false;
    const auto a=f.bridge.GetHistory().requestedHead;
    if(!Check(f.bridge.GetHistory().totalNodeCount==1&&f.bridge.GetShaderTickNeeded()&&f.service->GetIsInteracting(),
        "Released published a node before shader preparation"))return false;
    if(!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().appliedHead==a&&!f.service->GetIsInteracting(),"first release failed"))return false;
    if(!f.bridge.SetCropMode(CropRemovalMode::None)||f.bridge.GetShaderTickNeeded())return false;
    if(!f.bridge.SetCropMode(CropRemovalMode::RemoveInside)||!Flush(f.bridge,f.window))return false;
    const auto b=f.bridge.GetHistory().appliedHead;const auto root=f.bridge.GetHistory().rootNodeId;
    if(!Check(a!=b&&f.bridge.GetNode(a)->parentNodeId==root&&f.bridge.GetNode(b)->parentNodeId==root
        &&f.bridge.GetNode(a)->operation->removalMode==CropRemovalMode::KeepInside
        &&f.bridge.GetHistory().totalNodeCount==3,"mode change overwrote original operation"))return false;
    if(!f.bridge.SetCropNode(root)||!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().appliedHead==root&&f.bridge.GetCropHistory().nodeCount==0,
        "empty Root table was not applied"))return false;
    if(!Check(!f.bridge.NextCrop(),"ambiguous redo silently chose a branch"))return false;
    if(!f.bridge.SetCropNode(b)||!Flush(f.bridge,f.window))return false;
    const auto shaderRevision=f.service->GetEffectState().activeRevision;
    if(!Check(f.bridge.SetCropNode(b)&&!f.bridge.GetShaderTickNeeded()
        &&f.service->GetEffectState().activeRevision==shaderRevision&&!f.bridge.SetCropNode(0),"node selection identity/no-op"))return false;
    if(!f.bridge.PreviousCrop()||!Flush(f.bridge,f.window))return false;
    const auto zeroRevision=f.service->GetEffectState().activeRevision;const auto attaches=f.service->attachCount;
    if(!Check(f.bridge.ExitCrop()&&f.bridge.StartView(f.view)&&f.service->attachCount==attaches
        &&f.service->GetEffectState().activeRevision==zeroRevision,"Root reentry lost the zero-operation binding"))return false;
    return true;
}

bool GetCurvedWidgetHistory() {
    for(const auto shape:{CropShape::Sphere,CropShape::Cylinder}) {
        Fixture f;if(!f.ready)return false;
        const bool switched=shape==CropShape::Sphere?f.bridge.SwitchCropSphere():f.bridge.SwitchCropCylinder();
        if(!switched||!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
        f.window->Render();
        f.renderer->SetWorldPoint(1.5,shape==CropShape::Sphere?1.5:2.25,1.5,1);f.renderer->WorldToDisplay();
        const auto* point=f.renderer->GetDisplayPoint();const int x=static_cast<int>(point[0]),y=static_cast<int>(point[1]);
        f.interactor->SetEventPosition(x,y);f.interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
        f.interactor->SetEventPosition(x+10,y);f.interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
        f.interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
        if(!Flush(f.bridge,f.window))return false;
        const auto history=f.bridge.GetHistory();const auto node=f.bridge.GetNode(history.appliedHead);
        if(!Check(node&&node->operation&&node->operation->geometryType==shape&&history.totalNodeCount==2
            &&!f.service->GetIsInteracting(),"curve mouse release did not enter the common immutable history pipeline"))return false;
        if(!f.bridge.SetCropMode(CropRemovalMode::RemoveInside)||!Flush(f.bridge,f.window))return false;
        const auto sibling=f.bridge.GetNode(f.bridge.GetHistory().appliedHead);
        if(!Check(sibling&&sibling->parentNodeId==history.rootNodeId&&sibling->nodeId!=node->nodeId
            &&node->operation->removalMode==CropRemovalMode::KeepInside&&f.bridge.ExitCrop(),"curve mode replacement overwrote its original branch"))return false;
    }
    return true;
}

bool GetBranchesAndFrozenBuilds() {
    Fixture f;if(!f.ready)return false;const auto root=f.bridge.GetHistory().rootNodeId;
    const auto a=Append(f.bridge,root),b=Append(f.bridge,a.nodeId),c=Append(f.bridge,b.nodeId),d=Append(f.bridge,a.nodeId);
    if(!Check(a.isAccepted&&b.isAccepted&&c.isAccepted&&d.isAccepted&&Flush(f.bridge,f.window),"queued branches failed"))return false;
    if(!Check(f.bridge.GetHistory().totalNodeCount==5&&f.bridge.GetNode(c.nodeId)->parentNodeId==b.nodeId
        &&f.bridge.GetNode(d.nodeId)->parentNodeId==a.nodeId,"GPU queue truncated a branch"))return false;
    bool captured=false;
    if(!f.bridge.BuildCropResult(c.nodeId,[&](CropMaterializationCandidate result) {
        captured=result.isSucceeded&&result.sourceRevision==f.input.data->self&&result.operations.size()==3
            &&result.operations[0].operationIndex==a.nodeId&&result.operations[1].operationIndex==b.nodeId
            &&result.operations[2].operationIndex==c.nodeId;
    }))return false;
    const auto busy=Append(f.bridge,d.nodeId);
    if(!Check(!busy.isAccepted&&busy.failureReason==CropFailure::Busy&&Deliver(f.bridge)&&captured
        &&f.bridge.GetHistory().appliedHead==d.nodeId,"explicit build freezes branch C while editing branch D is locked"))return false;
    const auto e=Append(f.bridge,d.nodeId);
    if(!Check(e.isAccepted&&Flush(f.bridge,f.window)&&f.bridge.GetHistory().appliedHead==e.nodeId,
        "branch editing resumes after build completion"))return false;
    bool noOperations=false;
    if(!Check(!f.bridge.BuildCropResult(root,[&](CropMaterializationCandidate result) {noOperations=result.failureReason==CropFailure::NoCropOperations;})
        &&noOperations,"Root build did not distinguish NoCropOperations"))return false;
    const auto count=f.bridge.GetHistory().totalNodeCount;
    auto other=BuildGraphInput(f.data,f.image,f.input.inputModelBounds);
    if(!Check(!f.bridge.SetCropInput(other)&&f.bridge.GetSource().data->self==f.input.data->self
        &&f.bridge.GetHistory().totalNodeCount==count,"document Root changed to a later binding"))return false;
    bool cancelled=false;
    if(!f.bridge.BuildCropResult(c.nodeId,[&](CropMaterializationCandidate result) {cancelled=result.isCancelled&&!result.isSucceeded;})
        ||!f.bridge.CancelPending()||!Deliver(f.bridge))return false;
    return Check(cancelled&&f.bridge.GetHistory().totalNodeCount==count,"build cancellation destroyed history");
}

bool GetExitAndRebind() {
    Fixture f;if(!f.ready||!f.bridge.SwitchCropBox()||!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    if(!SendWidgetInput(f.renderer,f.interactor))return false;
    const auto requested=f.bridge.GetHistory().requestedHead;
    if(!Check(f.bridge.ExitCrop()&&f.bridge.GetShaderTickNeeded()&&Flush(f.bridge,f.window)
        &&f.bridge.GetHistory().appliedHead==requested&&!f.bridge.GetCropActive(),"Exit discarded an accepted release"))return false;
    const auto oldRevision=f.service->GetEffectState().activeRevision;const auto oldDetach=f.service->detachCount;
    if(!Check(f.bridge.StartView(f.view)&&!f.bridge.GetShaderTickNeeded()&&f.service->GetEffectState().activeRevision==oldRevision,
        "same-view reentry rebuilt committed history"))return false;
    auto occupied=std::make_shared<CropServiceStub>(RenderInputStamp{f.input.data->self},f.renderer);
    auto blockedView=f.view;blockedView.referenceService=occupied;blockedView.targetServices={occupied};
    CropBridge blocker;
    if(!blocker.StartView(blockedView)||!Check(!f.bridge.StartView(blockedView)&&f.service->detachCount==oldDetach
        &&f.bridge.GetHistory().appliedHead==requested,"failed target attachment changed current history"))return false;
    blocker.ClearBindings();
    if(!Check(f.bridge.StartView(blockedView)&&f.service->detachCount==oldDetach&&f.bridge.GetShaderTickNeeded(),
        "rebind retired old target before readiness"))return false;
    if(!Check(f.bridge.CancelPending()&&occupied->detachCount==2&&f.service->detachCount==oldDetach
        &&f.service->GetEffectState().activeRevision==oldRevision,"explicit rebind cancellation did not restore old target"))return false;
    if(!f.bridge.StartView(blockedView)||!Flush(f.bridge,f.window))return false;
    if(!Check(f.service->detachCount==oldDetach+1&&occupied->GetEffectState().status==RenderEffectStatus::Committed,
        "rebind did not atomically hand over targets"))return false;
    const auto history=f.bridge.GetHistory();
    if(!f.bridge.ClearBindings())return false;
    if(!Check(f.bridge.GetHistory().appliedHead==history.appliedHead&&f.bridge.GetHistory().totalNodeCount==history.totalNodeCount,
        "view detach destroyed the document"))return false;
    return Check(f.bridge.StartView(blockedView)&&Flush(f.bridge,f.window),"detached document did not replay history");
}

bool GetQueuedModesAndLag() {
    Fixture f;if(!f.ready||!f.bridge.SwitchCropBox()||!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    auto foreign=f.input.data->self;++foreign.generation;f.service->SetRenderInputStamp({foreign});
    if(!SendWidgetInput(f.renderer,f.interactor))return false;
    const auto original=f.bridge.GetHistory().requestedHead;
    if(!Check(f.bridge.GetHistory().totalNodeCount==1&&f.bridge.GetShaderTickNeeded()&&f.service->GetIsInteracting(),
        "lagging render input dropped a release"))return false;
    if(!f.bridge.SetCropMode(CropRemovalMode::RemoveInside))return false;
    const auto removed=f.bridge.GetHistory().requestedHead;
    if(!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    const auto kept=f.bridge.GetHistory().requestedHead;
    if(!f.service->SetRenderInputStamp({f.input.data->self})||!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().totalNodeCount==4&&f.bridge.GetHistory().appliedHead==kept
        &&f.bridge.GetNode(removed)->operation->removalMode==CropRemovalMode::RemoveInside
        &&f.bridge.GetNode(original)->operation->removalMode==CropRemovalMode::KeepInside&&!f.service->GetIsInteracting(),
        "queued modes were collapsed or mutated"))return false;
    if(!SendWidgetInput(f.renderer,f.interactor)||!Flush(f.bridge,f.window))return false;
    return Check(f.bridge.GetNode(f.bridge.GetHistory().appliedHead)->parentNodeId==kept,
        "next release used a stale mode replacement parent");
}

bool GetShapeSequenceAndRoot() {
    Fixture f;if(!f.ready||!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    const std::array<bool,8> planes={true,true,false,false,true,false,true,false};
    std::vector<CropNodeId> nodes;
    for(std::size_t index=0;index<planes.size();++index) {
        if(!(planes[index]?f.bridge.SwitchCropPlane():f.bridge.SwitchCropBox()))return false;
        f.window->Render();
        if(!(planes[index]?SendPlaneInput(f.renderer,f.interactor,8+static_cast<int>(index)):
            SendWidgetInput(f.renderer,f.interactor,8+static_cast<int>(index)))||!Flush(f.bridge,f.window))return false;
        nodes.push_back(f.bridge.GetHistory().appliedHead);
        if(f.bridge.GetHistory().totalNodeCount!=index+2) {
            std::cerr<<"Mixed sequence index="<<index<<" nodes="<<f.bridge.GetHistory().totalNodeCount
                <<" requested="<<f.bridge.GetHistory().requestedHead<<" applied="<<f.bridge.GetHistory().appliedHead<<'\n';
            return false;
        }
        if(!Check(f.bridge.GetNode(nodes.back())->operation->geometryType==(planes[index]?CropShape::Plane:CropShape::Box),
            "mixed widget sequence lost shape ordering"))return false;
    }
    for(std::size_t depth=8;depth>0;--depth)if(!f.bridge.PreviousCrop()||!Flush(f.bridge,f.window)
        ||f.bridge.GetCropHistory().nodeCount!=depth-1)return Check(false,"undo to Root skipped a node");
    const auto root=f.bridge.GetHistory().rootNodeId;const auto zeroRevision=f.service->GetEffectState().activeRevision;
    if(!f.bridge.ExitCrop()||!f.bridge.StartView(f.view)||!f.bridge.SwitchCropPlane()
        ||!f.bridge.SetCropMode(CropRemovalMode::KeepInside))return false;
    f.window->Render();
    if(!SendPlaneInput(f.renderer,f.interactor,18)||!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().totalNodeCount==10&&f.bridge.GetNode(nodes.back()).has_value()
        &&f.bridge.GetNode(f.bridge.GetHistory().appliedHead)->parentNodeId==root
        &&f.service->GetEffectState().activeRevision>zeroRevision,"Root append deleted eight-node redo branch"))return false;
    // 无有效移动、尚未 Released 的拖拽在 Exit 时不能进入命令队列。
    if(!f.bridge.SwitchCropBox())return false;f.window->Render();
    const auto count=f.bridge.GetHistory().totalNodeCount;
    if(!SendWidgetInput(f.renderer,f.interactor,8,false)||!f.bridge.ExitCrop())return false;
    f.interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
    return Check(!f.bridge.GetShaderTickNeeded()&&f.bridge.GetHistory().totalNodeCount==count&&!f.service->GetIsInteracting(),
        "Exit during an unfinished drag created history");
}

bool GetPruneAndMultiviewFailure() {
    Fixture f;if(!f.ready)return false;
    auto second=std::make_shared<CropServiceStub>(RenderInputStamp{f.input.data->self},f.renderer);
    auto multi=f.view;multi.targetServices.push_back(second);
    if(!f.bridge.StartView(multi))return false;
    const auto root=f.bridge.GetHistory().rootNodeId;
    const auto a=Append(f.bridge,root);if(!Flush(f.bridge,f.window))return false;
    const auto aRevision=f.service->GetEffectState().activeRevision;
    const auto b=Append(f.bridge,a.nodeId);
    auto prune=Request(f.bridge,CropEditKind::Prune,0);prune.prune.nodeIds={a.nodeId};
    prune.prune.fallback=CropPruneFallback::NearestSurvivingAncestor;
    if(!Check(!f.bridge.SendRequest(prune).isAccepted,"prune bypassed an accepted preview"))return false;
    // 一个目标在 prepare 期间失去输入；其它目标必须保持此前已应用版本。
    auto foreign=f.input.data->self;++foreign.generation;second->SetRenderInputStamp({foreign});
    f.window->Render();f.bridge.SendShaderCommit();
    if(!Check(f.bridge.GetOutcome(b.requestId)->status==CropEditStatus::Failed&&!f.bridge.GetNode(b.nodeId)
        &&f.bridge.GetHistory().appliedHead==a.nodeId&&f.service->GetEffectState().activeRevision==aRevision,
        "failed required View partially committed history"))return false;
    second->SetRenderInputStamp({f.input.data->self});
    // 换一组干净 binding 重放，保留失败前树；再检查 Root 零 uniform 同步提交。
    if(!f.bridge.ClearBindings()||!f.bridge.StartView(multi)||!Flush(f.bridge,f.window))return false;
    CropResultRecord result;result.resultId=CropHistory::CreateNodeId();result.nodeId=a.nodeId;
    result.status=CropResultStatus::Building;result.sourceRevision=f.input.data->self;
    f.bridge.SetResults({result});
    prune=Request(f.bridge,CropEditKind::Prune,0);prune.prune.nodeIds={a.nodeId};prune.prune.fallback=CropPruneFallback::NearestSurvivingAncestor;
    if(!Check(f.bridge.GetPruneImpact(prune.prune).failureReason==CropFailure::Busy
        &&!f.bridge.SendRequest(prune).isAccepted,"Building path allowed prune"))return false;
    f.bridge.SetResults({});
    prune.expectedRevision=f.bridge.GetHistory().stateRevision;prune.requestId=CropHistory::CreateNodeId();
    const auto accepted=f.bridge.SendRequest(prune);if(!accepted.isAccepted)return false;
    if(!Check(f.bridge.GetNode(a.nodeId).has_value(),"prune deleted before fallback preview"))return false;
    f.window->Render();const auto dirty1=f.service->dirtyCount,dirty2=second->dirtyCount;
    if(!f.bridge.SendShaderCommit())return false;
    return Check(!f.bridge.GetNode(a.nodeId)&&f.bridge.GetHistory().appliedHead==root
        &&f.service->dirtyCount==dirty1+1&&second->dirtyCount==dirty2+1
        &&f.bridge.GetOutcome(accepted.requestId)->prune.deletedCount==1,
        "all-View Root fallback/prune commit failed");
}

bool GetActualRenderedHead() {
    Fixture f;if(!f.ready)return false;
    const auto wait=[&](const std::function<bool()>& done) {
        for(int poll=0;poll<1000;++poll) {
            RenderFrameLifetime::PollAll();if(done())return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    };
    const auto root=f.bridge.GetHistory().rootNodeId;const auto a=Append(f.bridge,root);
    if(!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().appliedHead==a.nodeId&&f.bridge.GetHistory().renderedHead!=a.nodeId,
        "shader commit was mistaken for a rendered frame"))return false;
    const auto pending=f.bridge.GetViewState(f.service.get());
    if(!Check(pending&&pending->appliedHead==a.nodeId&&pending->requestedHead==a.nodeId
        &&pending->renderedHead!=a.nodeId&&pending->isRenderPending&&!f.bridge.GetViewState(nullptr),
        "per-view state reported a committed node as already presented"))return false;
    f.window->Render();
    if(!Check(wait([&]{return f.bridge.GetHistory().renderedHead==a.nodeId;}),"presented GPU frame did not map back to its node"))return false;
    const auto presented=f.bridge.GetViewState(f.service.get());
    if(!Check(presented&&presented->renderedHead==a.nodeId&&!presented->isRenderPending,
        "per-view state did not observe its completed frame"))return false;
    auto editB=Request(f.bridge,CropEditKind::Append,a.nodeId);editB.operation.geometryType=CropShape::Plane;
    editB.operation.planeNormalInInputModel={1,0,0};editB.operation.planeCenterInInputModel={0.3,0,0};
    const auto b=f.bridge.SendRequest(editB);if(!b||!Flush(f.bridge,f.window))return false;
    if(!Check(f.bridge.GetHistory().appliedHead==b.nodeId&&f.bridge.GetHistory().renderedHead==a.nodeId,
        "applied head did not remain separate from the last rendered head"))return false;
    if(!Check(f.service->GetPointVisible({0.25,0,0}),"unpresented edit changed business picking before its frame"))return false;
    const std::vector<CropVectorDouble3Array> samples{{0.2,0,0},{0.3,0,0},{0.4,0,0}};
    const auto oldPrecision=f.bridge.GetPreviewPrecision(f.service.get(),samples);
    if(!Check(oldPrecision.failureReason==CropFailure::None&&oldPrecision.renderedHead==a.nodeId
        &&oldPrecision.keptCount==3,"precision query adopted an unpresented node"))return false;
    f.window->SwapBuffersOff();f.window->Render();
    if(!wait([&]{return !f.service->GetEffectState().isRenderPending;}))return false;
    if(!Check(f.bridge.GetHistory().renderedHead==a.nodeId,"back-buffer validation was reported as presented"))return false;
    if(!Check(f.service->GetPointVisible({0.25,0,0}),"candidate/back-buffer frame changed picking"))return false;
    f.window->SwapBuffersOn();f.window->Render();
    if(!wait([&]{return f.bridge.GetHistory().renderedHead==b.nodeId;}))return false;
    if(!Check(!f.service->GetPointVisible({0.25,0,0})&&f.service->GetPointVisible({0.4,0,0}),
        "presented crop did not reject its hidden side for business picking"))return false;
    const auto precision=f.bridge.GetPreviewPrecision(f.service.get(),samples);
    if(!Check(precision.failureReason==CropFailure::None&&precision.renderedHead==b.nodeId
        &&precision.coordinates.isAvailable&&precision.keptCount==1&&precision.removedCount==1
        &&precision.boundaryBandCount==1&&precision.precisionNotMetCount==0
        &&f.bridge.GetPreviewPrecision(f.service.get(),std::vector<CropVectorDouble3Array>(257)).failureReason==CropFailure::ResourceLimit,
        "presented precision query failed kept/removed/band or sample budget"))return false;
    auto tiny=Request(f.bridge,CropEditKind::Append,b.nodeId);tiny.operation.geometryType=CropShape::Sphere;
    tiny.operation.centerInInputModel={0.4,0,0};tiny.operation.radius=1e-10;
    const auto rejected=f.bridge.SendRequest(tiny);if(!rejected)return false;
    f.window->Render();(void)f.bridge.SendShaderCommit();
    const auto outcome=f.bridge.GetOutcome(rejected.requestId);
    if(!Check(outcome&&outcome->failureReason==CropFailure::PrecisionNotMet
        &&!f.bridge.GetNode(rejected.nodeId)&&f.bridge.GetHistory().appliedHead==b.nodeId,
        "unresolvable preview advanced history or lost PrecisionNotMet"))return false;
    auto failFrame=vtkSmartPointer<vtkCallbackCommand>::New();
    failFrame->SetCallback([](vtkObject* source,unsigned long,void*,void*){source->InvokeEvent(vtkCommand::ErrorEvent);});
    const auto failedTag=f.window->AddObserver(vtkCommand::EndEvent,failFrame,1.0);
    f.window->Render();f.window->RemoveObserver(failedTag);
    if(!Check(wait([&]{return f.bridge.GetHistory().renderedHead==0;})&&!f.service->GetPointVisible({0.4,0,0}),
        "failed presented frame retained a stale known predicate for picking"))return false;
    f.window->Render();if(!wait([&]{return f.bridge.GetHistory().renderedHead==b.nodeId;}))return false;
    auto different=f.input.data->self;++different.generation;
    if(!f.service->SetRenderInputStamp({different}))return false;
    auto prepared=f.bridge.BuildSourceCommit(root,false);
    if(!prepared||!f.service->StartCandidate({f.input.data->self}))return false;
    f.window->SwapBuffersOff();f.window->Render();f.window->SwapBuffersOn();
    RenderFrameLifetime::PollAll();
    if(!f.service->SetCandidateView()||!f.bridge.GetSourceCommitReady(*prepared))return false;
    f.bridge.SetSourceCommit(std::move(*prepared));f.service->CompleteCandidate();
    if(!Check(f.bridge.GetHistory().appliedHead==root&&f.bridge.GetHistory().renderedHead!=root,
        "candidate replay incorrectly became a rendered Root on adoption"))return false;
    if(!Check(!f.service->GetPointVisible({0.25,0,0}),"Root adoption opened picking before a Root frame was presented"))return false;
    f.window->Render();
    return Check(wait([&]{return f.bridge.GetHistory().renderedHead==root;})&&f.service->GetPointVisible({0.25,0,0}),"zero-node Root uniform did not produce a completed Root frame");
}

bool GetArchiveSourceValidation() {
    Fixture f;if(!f.ready)return false;
    const auto originalRoot=f.bridge.GetHistory().rootNodeId;
    const auto a=Append(f.bridge,originalRoot);if(!a||!Flush(f.bridge,f.window))return false;
    const auto archive=f.bridge.GetArchive();
    if(!archive.imageGeometry||archive.sourceType!=f.input.data->type||archive.coordinateFrame.empty()||archive.maskSourceRevision)return false;
    const auto rejected=[&](CropDocumentArchive bad,CropFailure expected) {
        CropBridge candidate;if(!candidate.SetCropInput(f.input))return false;const auto before=candidate.GetHistory();
        std::vector<CropNodeMapping> mappings;
        return candidate.SetArchive(bad,mappings)==expected&&candidate.GetHistory().documentId==before.documentId
            &&candidate.GetHistory().rootNodeId==before.rootNodeId&&candidate.GetHistory().totalNodeCount==1&&mappings.empty();
    };
    auto bad=archive;bad.imageGeometry->spacing[0]*=2;
    if(!Check(rejected(bad,CropFailure::SourceMismatch),"archive with changed Root geometry was accepted"))return false;
    bad=archive;bad.maskSourceRevision=archive.sourceRevision;
    if(!Check(rejected(bad,CropFailure::SourceMismatch),"archive with changed Root mask identity was accepted"))return false;
    bad=archive;bad.coordinateFrame="LPS";
    if(!Check(rejected(bad,CropFailure::SourceMismatch),"archive with changed coordinate frame was accepted"))return false;
    bad=archive;bad.nodes.back().operation->geometryType=static_cast<CropShape>(99);
    if(!Check(rejected(bad,CropFailure::BadInput),"archive with unknown geometry was accepted"))return false;
    CropBridge restored;if(!restored.SetCropInput(f.input))return false;std::vector<CropNodeMapping> mappings;
    if(restored.SetArchive(archive,mappings)!=CropFailure::None)return false;
    const auto history=restored.GetHistory();
    const auto mapped=std::find_if(mappings.begin(),mappings.end(),[&](const auto& item){return item.archivedNodeId==a.nodeId;});
    if(!Check(history.documentId!=f.bridge.GetHistory().documentId&&history.rootNodeId!=originalRoot
        &&mappings.size()==archive.nodes.size()&&mapped!=mappings.end()&&mapped->nodeId==history.appliedHead
        &&restored.GetNode(mapped->nodeId)->parentNodeId==history.rootNodeId&&history.results.empty(),
        "archive did not allocate fresh runtime identities and preserve its parent relation"))return false;
    auto same=archive;
    if(!CropHistory::GetArchivesSame(archive,same))return false;
    same.nodes.back().operation->height+=1;
    if(!Check(!CropHistory::GetArchivesSame(archive,same),"archive request equality ignored a serialized geometry field"))return false;
    auto mask=vtkSmartPointer<vtkImageData>::New();mask->CopyStructure(f.input.image->image);mask->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    const auto count=static_cast<std::size_t>(mask->GetNumberOfPoints());auto* bytes=static_cast<unsigned char*>(mask->GetScalarPointer());
    std::fill(bytes,bytes+count,255);bytes[0]=0;
    TestDataPort maskedData;const auto maskedView=maskedData.SetPrimaryImage(f.input.image->image,mask);
    if(!maskedView)return false;
    auto maskedInput=f.input;maskedInput.graph=maskedView->graph;maskedInput.binding=maskedView->binding;
    maskedInput.data=maskedView->data;maskedInput.image=maskedView;
    CropBridge maskedSource,maskedRestored;
    if(!maskedSource.SetCropInput(maskedInput)||!maskedRestored.SetCropInput(maskedInput))return false;
    const auto maskedArchive=maskedSource.GetArchive();
    std::vector<CropNodeMapping> maskedMappings;
    return Check(maskedArchive.maskSourceRevision==maskedArchive.sourceRevision
        &&maskedRestored.SetArchive(maskedArchive,maskedMappings)==CropFailure::None
        &&maskedRestored.GetArchive().maskSourceRevision==maskedArchive.maskSourceRevision
        &&maskedRestored.GetSource().image->validityMask->GetScalarComponentAsDouble(0,0,0,0)==0,
        "masked Root archive failed to preserve its exact mask identity and domain");
}

bool GetSourcePreviewCommit() {
    Fixture f;if(!f.ready)return false;
    const auto root=f.bridge.GetHistory().rootNodeId;
    const auto a=Append(f.bridge,root);if(!Flush(f.bridge,f.window))return false;
    auto other=f.input.data->self;++other.generation;
    if(!f.service->SetRenderInputStamp({other}))return false;
    const auto oldState=f.service->GetCurrentState();const auto oldHead=f.bridge.GetHistory().appliedHead;
    auto abandoned=f.bridge.BuildSourceCommit(root,false);
    if(!Check(abandoned&&f.bridge.GetSourceCommitReady(*abandoned)&&f.service->StartCandidate({f.input.data->self}),
        "Root candidate preparation"))return false;
    f.window->Render();
    if(!Check(f.service->GetCandidateState().status==RenderEffectStatus::Committed
        &&f.service->GetCurrentState().activeRevision==oldState.activeRevision
        &&f.bridge.GetHistory().appliedHead==oldHead,"candidate preview changed current source/head"))return false;
    f.service->ClearCandidate();abandoned.reset();
    if(!Check(f.bridge.GetHistory().appliedHead==a.nodeId&&f.service->GetCurrentState().activeRevision==oldState.activeRevision,
        "abandoned source candidate changed applied state"))return false;
    auto prepared=f.bridge.BuildSourceCommit(root,false);
    if(!prepared||!f.service->StartCandidate({f.input.data->self}))return false;
    f.window->Render();
    if(!f.service->SetCandidateView()||!f.bridge.GetSourceCommitReady(*prepared))return false;
    // Host 的数据提交线性化点之后，即使业务 lease 刚停也必须完成无失败接管。
    if(!f.lease->StopLease())return false;
    f.bridge.SetSourceCommit(std::move(*prepared));f.service->CompleteCandidate();
    return Check(f.bridge.GetHistory().appliedHead==root&&f.bridge.GetHistory().totalNodeCount==2
        &&f.service->GetEffectState().activeRevision==f.service->GetCurrentState().activeRevision,
        "source commit did not adopt prepared Root without discarding history");
}

bool GetStoppedLeaseCleanup() {
    Fixture f;if(!f.ready)return false;
    const auto a=Append(f.bridge,f.bridge.GetHistory().rootNodeId);if(!Flush(f.bridge,f.window))return false;
    const auto b=Append(f.bridge,a.nodeId);
    if(!f.lease->StopLease()||!f.bridge.ClearBindings())return false;
    if(!Check(f.bridge.GetOutcome(b.requestId)->status==CropEditStatus::Cancelled&&f.bridge.GetHistory().appliedHead==a.nodeId,
        "stopped lease could not finalize accepted request cancellation"))return false;
    return Check(f.bridge.ClearDocument()&&f.bridge.GetHistory().documentId==0,"empty closed document retained Root");
}
}

int CropBridgeSuite::GetFailCount() const
{
    int failures=0;
    const auto run=[&](bool result,const char* name){if(!result){std::cerr<<"Bridge scenario failed: "<<name<<'\n';++failures;}};
    run(GetWidgetAndSiblingEdits(),"widget edits and sibling replacement");
    run(GetCurvedWidgetHistory(),"curved widgets and immutable history");
    run(GetBranchesAndFrozenBuilds(),"branches and fixed Root materialization");
    run(GetExitAndRebind(),"Exit, reentry and transactional target replacement");
    run(GetQueuedModesAndLag(),"pending release and every queued mode");
    run(GetShapeSequenceAndRoot(),"mixed shape sequence and Root redo preservation");
    run(GetPruneAndMultiviewFailure(),"protected prune and required View failure");
    run(GetActualRenderedHead(),"applied, back-buffer and GPU-rendered node identities");
    run(GetArchiveSourceValidation(),"archive source validation and runtime node mapping");
    run(GetSourcePreviewCommit(),"candidate source replay and no-fail adoption");
    run(GetStoppedLeaseCleanup(),"stopped lease cancellation and document cleanup");
    return failures;
}
