#include "Render/Contracts/RenderStrategyFactory.h"

#include "Render/Strategies/CompositeStrategy.h"
#include "Render/Strategies/IsoSurfaceStrategy.h"
#include "Render/Strategies/SliceStrategy.h"
#include "Render/Strategies/VolumeStrategy.h"

std::shared_ptr<AbstractVisualStrategy>
CreateRenderStrategy(const VizMode mode)
{
    return CreateRenderStrategy(mode, nullptr);
}

std::shared_ptr<AbstractVisualStrategy>
CreateRenderStrategy(
    const VizMode mode,
    const std::shared_ptr<RenderStrategyServices>& services)
{
    switch (mode) {
    case VizMode::Volume:
        return std::make_shared<VolumeStrategy>(services);
    case VizMode::IsoSurface:
        return std::make_shared<IsoSurfaceStrategy>(services);
    case VizMode::SliceTop_down:
        return std::make_shared<SliceStrategy>(Orientation::Top_down);
    case VizMode::SliceFront_back:
        return std::make_shared<SliceStrategy>(Orientation::Front_back);
    case VizMode::SliceLeft_right:
        return std::make_shared<SliceStrategy>(Orientation::Left_right);
    case VizMode::CompositeVolume:
        return std::make_shared<CompositeStrategy>(
            std::make_shared<VolumeStrategy>(services));
    case VizMode::CompositeIsoSurface:
        return std::make_shared<CompositeStrategy>(
            std::make_shared<IsoSurfaceStrategy>(services));
    default:
        return nullptr;
    }
}


#include "Render/Support/BaseVisualStrategy.h"
#include <vtkOpenGLPolyDataMapper.h>
#include <vtkObjectFactory.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <cmath>

namespace {
class MeshEffectMapper final : public vtkOpenGLPolyDataMapper {
public:
    static MeshEffectMapper* New();
    vtkTypeMacro(MeshEffectMapper,vtkOpenGLPolyDataMapper);
    RenderEffectBinding* binding=nullptr;
    void RenderPiece(vtkRenderer* renderer,vtkActor* actor) override {
        if(binding&&!binding->OnRenderStart(renderer)){(void)binding->OnRenderStop();return;}
        this->Superclass::RenderPiece(renderer,actor);
        if(binding)(void)binding->OnRenderStop();
    }
};
vtkStandardNewMacro(MeshEffectMapper);

class MeshStrategy final : public BaseVisualStrategy {
public:
    MeshStrategy() {
        mapper=vtkSmartPointer<MeshEffectMapper>::New();actor=vtkSmartPointer<vtkActor>::New();
        mapper->SetVBOShiftScaleMethod(vtkOpenGLPolyDataMapper::DISABLE_SHIFT_SCALE);
        mapper->ScalarVisibilityOff();actor->SetMapper(mapper);actor->PickableOff();AttachProp(actor);
    }
    ~MeshStrategy() override {ClearRenderBinding();}
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {mapper->SetInputData(vtkPolyData::SafeDownCast(data));}
    bool SetInputData(vtkSmartPointer<vtkDataObject> data,vtkSmartPointer<vtkImageData> mask) override {
        auto* mesh=vtkPolyData::SafeDownCast(data);if(!mesh||mask||mesh->GetNumberOfPoints()==0)return false;
        mapper->SetInputData(mesh);return true;
    }
    vtkProp3D* GetMainProp() override {return actor;}
    bool SetVisualState(const RenderParams& params,UpdateFlags flags) override {
        if((flags&UpdateFlags::Material)!=UpdateFlags::None) {
            const auto& m=params.material;
            for(const auto value:{m.ambient,m.diffuse,m.specular,m.opacity})if(!std::isfinite(value)||value<0||value>1)return false;
            if(!std::isfinite(m.specularPower)||m.specularPower<0)return false;
            auto* p=actor->GetProperty();p->SetAmbient(m.ambient);p->SetDiffuse(m.diffuse);p->SetSpecular(m.specular);
            p->SetOpacity(m.opacity);p->SetSpecularPower(m.specularPower);
            if(m.isShadeOn)p->SetInterpolationToPhong();else p->SetInterpolationToFlat();
        }
        if((flags&UpdateFlags::Transform)!=UpdateFlags::None)Set3DPropsTransform(params.modelMatrix);
        return true;
    }
protected:
    RenderEffectTarget GetRenderEffectTarget() const override {
        RenderEffectTarget target;target.targetKind=RenderTargetKind::PolyData;
        target.mapper=mapper;target.shaderProperty=actor->GetShaderProperty();return target;
    }
    void SetEffectBinding(RenderEffectBinding* value) override {mapper->binding=value;}
private:
    vtkSmartPointer<MeshEffectMapper> mapper;
    vtkSmartPointer<vtkActor> actor;
};
}
std::shared_ptr<AbstractVisualStrategy> CreateMeshRenderStrategy(){return std::make_shared<MeshStrategy>();}
