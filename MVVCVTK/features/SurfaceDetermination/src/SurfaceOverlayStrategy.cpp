#include "SurfaceOverlayStrategy.h"
#include "Render/Support/SliceContourPlane.h"

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

void SetActorStyle(vtkActor& actor, bool isSlice, bool isPreview)
{
    actor.GetProperty()->SetColor(isPreview ? 0.15 : isSlice ? 1.0 : 0.83,
        isPreview ? 0.85 : isSlice ? 1.0 : 0.84, isPreview ? 1.0 : isSlice ? 1.0 : 0.86);
    actor.GetProperty()->SetOpacity(isSlice ? 1.0 : isPreview ? 0.35 : 0.92);
    actor.GetProperty()->SetLighting(!isSlice && !isPreview);
    actor.GetProperty()->SetAmbient(0.35);
    actor.GetProperty()->SetDiffuse(0.65);
    if (isPreview && !isSlice) actor.GetProperty()->SetRepresentationToWireframe();
    actor.SetPickable(false);
}

} // namespace

SurfaceOverlayStrategy::SurfaceOverlayStrategy(bool isPreview)
    : m_actor(vtkSmartPointer<vtkActor>::New())
    , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
{
    m_mapper->ScalarVisibilityOff();
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetMapper(m_mapper);
    SetActorStyle(*m_actor, false, isPreview);
    AttachProp(m_actor);
    SetCaption(isPreview ? u8"表面测定：预览" : u8"表面测定：正式结果", 0);
    SetCaptionVisible(false);
}

void SurfaceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_mapper->SetInputData(surface);
    SetCaptionVisible(surface->GetNumberOfPoints() > 0);
}

void SurfaceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    Set3DPropsTransform(state.modelToWorld);
}

SurfaceSliceOverlayStrategy::SurfaceSliceOverlayStrategy(
    std::array<double, 3> normalModel, bool isPreview)
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
    SetActorStyle(*m_actor, true, isPreview);
    m_actor->GetProperty()->SetLineWidth(2.0F);
    AttachProp(m_actor);
    SetCaption(isPreview ? u8"表面测定：预览" : u8"表面测定：正式结果", 0);
    SetCaptionVisible(false);
}

void SurfaceSliceOverlayStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* surface = vtkPolyData::SafeDownCast(data);
    if (!surface) return;
    m_cutter->SetInputData(surface);
    SetCaptionVisible(surface->GetNumberOfPoints() > 0);
}

void SurfaceSliceOverlayStrategy::SetOverlayState(
    const FeatureOverlayState& state)
{
    (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_normalModel,state.modelToWorld);
    Set3DPropsTransform(state.modelToWorld);
}
