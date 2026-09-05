#include "AlignmentOverlay.h"
#include "AlignmentMath.h"
#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkUnsignedCharArray.h>

AlignmentOverlay::AlignmentOverlay()
    : m_actor(vtkSmartPointer<vtkActor>::New()),
      m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New()) {
    m_mapper->SetScalarModeToUseCellData();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetLighting(false);
    m_actor->GetProperty()->SetLineWidth(2.0F);
    m_actor->SetPickable(false);
    AttachProp(m_actor);
}
void AlignmentOverlay::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (auto *poly = vtkPolyData::SafeDownCast(data))
        m_mapper->SetInputData(poly);
}
void AlignmentOverlay::SetOverlayState(const FeatureOverlayState &state) {
    Set3DPropsTransform(state.modelToWorld);
}
vtkSmartPointer<vtkPolyData> AlignmentOverlay::BuildData(
    const AlignmentMatrix &sourceToTarget, const std::vector<AlignmentGeometry> &geometries,
    const AlignmentRecipe &recipe, double axisLength) {
    using namespace AlignmentMath;
    if (!Rigid(sourceToTarget) || !std::isfinite(axisLength) || axisLength <= 0)
        return {};
    const auto inverse = Inverse(sourceToTarget);
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetNumberOfComponents(3);
    const auto add = [&](const Vec &a, const Vec &b, std::array<unsigned char, 3> color) {
        const vtkIdType ids[]{points->InsertNextPoint(a.val), points->InsertNextPoint(b.val)};
        lines->InsertNextCell(2, ids);
        colors->InsertNextTypedTuple(color.data());
    };
    const Vec origin = Transform(inverse, {});
    for (int axis = 0; axis < 3; ++axis) {
        Vec p{};
        p[axis] = axisLength;
        std::array<unsigned char, 3> color{50, 50, 50};
        color[axis] = 255;
        add(origin, Transform(inverse, p), color);
    }
    for (const auto &g : geometries) {
        const auto center = Vector(g.sourceCenter), normal = Vector(g.sourceDirection);
        const auto a = Tangent(normal) * axisLength * 0.2, b = normal.cross(a);
        if (g.kind == AlignmentGeometryKind::Plane) {
            const std::array<Vec, 4> corners{center - a - b, center + a - b, center + a + b,
                                             center - a + b};
            for (std::size_t i = 0; i < 4; ++i)
                add(corners[i], corners[(i + 1) % 4], {200, 180, 70});
        } else if (g.kind == AlignmentGeometryKind::Line ||
                   g.kind == AlignmentGeometryKind::Cylinder)
            add(center - normal * axisLength * 0.3, center + normal * axisLength * 0.3,
                {200, 180, 70});
        else {
            add(center - a, center + a, {200, 180, 70});
            add(center - b, center + b, {200, 180, 70});
        }
    }
    for (const auto &constraint : recipe.constraints) {
        if (constraint.geometryIndex >= geometries.size())
            continue;
        const auto &g = geometries[constraint.geometryIndex];
        if (g.kind == AlignmentGeometryKind::Point || g.kind == AlignmentGeometryKind::Sphere ||
            g.kind == AlignmentGeometryKind::Circle)
            add(Vector(g.sourceCenter), Transform(inverse, Vector(constraint.nominalPoint)),
                {255, 100, 220});
    }
    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetLines(lines);
    data->GetCellData()->SetScalars(colors);
    return data;
}
