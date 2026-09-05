#include "SurfaceOverlayStrategy.h"

#include <vtkActor.h>
#include <vtkCutter.h>
#include <vtkDataObject.h>
#include <vtkPlane.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>

#include <cmath>
#include <limits>
#include <utility>

namespace {

bool SetNormalized(std::array<double, 3>& normal)
{
    const double length = std::sqrt(
        normal[0] * normal[0]
        + normal[1] * normal[1]
        + normal[2] * normal[2]);
    if (!std::isfinite(length)
        || length <= std::numeric_limits<double>::epsilon()) {
        return false;
    }
    for (double& value : normal) value /= length;
    return true;
}

void SetActorStyle(vtkActor& actor)
{
    actor.GetProperty()->SetColor(1.0, 0.55, 0.10);
    actor.GetProperty()->SetOpacity(0.92);
    actor.GetProperty()->SetLighting(false);
    actor.SetPickable(false);
}

} // namespace

SurfaceOverlayStrategy::SurfaceOverlayStrategy()
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
{
    m_mapper->ScalarVisibilityOff();
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetMapper(m_mapper);
    SetActorStyle(*m_actor);
    AttachProp(m_actor);
}

void SurfaceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_mapper->SetInputData(surface);
}

void SurfaceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    Set3DPropsTransform(state.modelToWorld);
}

SurfaceSliceOverlayStrategy::SurfaceSliceOverlayStrategy(
    std::array<double, 3> normalModel)
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_cutter(vtkSmartPointer<vtkCutter>::New())
    , m_plane(vtkSmartPointer<vtkPlane>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
    , m_normalModel(std::move(normalModel))
{
    if (!SetNormalized(m_normalModel)) {
        m_normalModel = { 0.0, 0.0, 1.0 };
    }
    m_plane->SetNormal(m_normalModel.data());
    m_cutter->SetCutFunction(m_plane);
    m_cutter->GenerateTrianglesOff();
    m_mapper->SetInputConnection(m_cutter->GetOutputPort());
    m_mapper->ScalarVisibilityOff();
    m_actor->SetMapper(m_mapper);
    SetActorStyle(*m_actor);
    m_actor->GetProperty()->SetLineWidth(2.0F);
    AttachProp(m_actor);
}

void SurfaceSliceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_cutter->SetInputData(surface);
}

void SurfaceSliceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    m_plane->SetOrigin(state.cursor.data());
    m_plane->SetNormal(m_normalModel.data());
    Set3DPropsTransform(state.modelToWorld);
}
