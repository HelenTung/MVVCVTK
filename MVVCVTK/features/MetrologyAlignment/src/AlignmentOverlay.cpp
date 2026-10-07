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

AlignmentOverlay::AlignmentOverlay(AlignmentMethod method)
    : m_actor(vtkSmartPointer<vtkActor>::New()),
      m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New()) {
    m_mapper->SetScalarModeToUseCellData();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetLighting(false);
    m_actor->GetProperty()->SetLineWidth(2.5F);
    m_actor->GetProperty()->SetPointSize(5);
    m_actor->GetProperty()->RenderPointsAsSpheresOn();
    m_mapper->SetColorModeToDirectScalars();
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetPickable(false);
    AttachProp(m_actor);
    const char* caption = u8"计量对齐：基准与约束";
    switch (method) {
    case AlignmentMethod::SequentialPlanes: caption = u8"计量对齐：依次拟合平面"; break;
    case AlignmentMethod::PlaneTwoHoles: caption = u8"计量对齐：一面两孔"; break;
    case AlignmentMethod::Rps: caption = u8"计量对齐：参考点系统"; break;
    case AlignmentMethod::ConstrainedBestFit: caption = u8"计量对齐：约束最佳拟合"; break;
    }
    SetCaption(caption, 5);
    SetCaptionVisible(false);
}
void AlignmentOverlay::SetInputData(vtkSmartPointer<vtkDataObject> data) {
    if (auto *poly = vtkPolyData::SafeDownCast(data)) {
        m_mapper->SetInputData(poly);
        SetCaptionVisible(poly->GetNumberOfPoints() > 0);
    }
}
void AlignmentOverlay::SetOverlayState(const FeatureOverlayState &state) {
    Set3DPropsTransform(state.modelToWorld);
}
vtkSmartPointer<vtkPolyData> AlignmentOverlay::BuildData(
    const AlignmentMatrix &sourceToTarget, const std::vector<AlignmentGeometry> &geometries,
    const AlignmentRecipe &recipe, double axisLength, const std::vector<double>* modelVertices) {
    using namespace AlignmentMath;
    if (!Rigid(sourceToTarget) || !std::isfinite(axisLength) || axisLength <= 0)
        return {};
    const auto inverse = Inverse(sourceToTarget);
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    auto markers = vtkSmartPointer<vtkCellArray>::New();
    auto markerColors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    markerColors->SetNumberOfComponents(3);
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
        std::array<unsigned char, 3> color{90, 90, 90};
        color[axis] = 255;
        add(origin, Transform(inverse, p), color);
    }
    for (const auto &g : geometries) {
        const auto center = Vector(g.sourceCenter), normal = Vector(g.sourceDirection);
        if (g.kind == AlignmentGeometryKind::Point) {
            // 拟合/约束位置用点标识，不把每个对应点伪装成一个坐标十字。
            const vtkIdType marker = points->InsertNextPoint(center.val);
            markers->InsertNextCell(1, &marker);
            const unsigned char color[]{200, 180, 70};
            markerColors->InsertNextTypedTuple(color);
            continue;
        }
        const auto a = Tangent(normal) * axisLength * 0.2, b = normal.cross(a);
        // 圆、球和圆柱的显示半径直接来自已拟合结果，不再拟合或制造残差。
        if ((g.kind == AlignmentGeometryKind::Circle || g.kind == AlignmentGeometryKind::Sphere
                || g.kind == AlignmentGeometryKind::Cylinder) && std::isfinite(g.radius) && g.radius > 0) {
            const auto u = Tangent(normal) * g.radius, v = normal.cross(u);
            const auto ring = [&](const Vec& c, const Vec& x, const Vec& y) {
                constexpr double tau = 6.283185307179586;
                for (int i = 0; i < 64; ++i) {
                    const double t0 = tau * i / 64, t1 = tau * (i + 1) / 64;
                    add(c + x * std::cos(t0) + y * std::sin(t0),
                        c + x * std::cos(t1) + y * std::sin(t1), {70, 220, 235});
                }
            };
            ring(center, u, v);
            if (g.kind == AlignmentGeometryKind::Sphere) {
                ring(center, u, normal * g.radius); ring(center, v, normal * g.radius);
            } else if (g.kind == AlignmentGeometryKind::Cylinder) {
                ring(center - normal * axisLength * 0.3, u, v);
                ring(center + normal * axisLength * 0.3, u, v);
                add(center - normal * axisLength * 0.3 + u,
                    center + normal * axisLength * 0.3 + u, {70, 220, 235});
                add(center - normal * axisLength * 0.3 - u,
                    center + normal * axisLength * 0.3 - u, {70, 220, 235});
            }
        }
        if (g.kind == AlignmentGeometryKind::Plane) {
            const std::array<Vec, 4> corners{center - a - b, center + a - b, center + a + b,
                                             center - a + b};
            for (std::size_t i = 0; i < 4; ++i)
                add(corners[i], corners[(i + 1) % 4], {200, 180, 70});
        } else if (g.kind == AlignmentGeometryKind::Line ||
                   g.kind == AlignmentGeometryKind::Cylinder)
            add(center - normal * axisLength * 0.3, center + normal * axisLength * 0.3,
                {200, 180, 70});
    }
    for (const auto &constraint : recipe.constraints) {
        if (constraint.geometryIndex >= geometries.size())
            continue;
        const auto &g = geometries[constraint.geometryIndex];
        if (g.kind == AlignmentGeometryKind::Point || g.kind == AlignmentGeometryKind::Sphere ||
            g.kind == AlignmentGeometryKind::Circle) {
            const auto source = Vector(g.sourceCenter);
            const auto target = Transform(inverse, Vector(constraint.nominalPoint));
            if ((source - target).dot(source - target) > 0)
                add(source, target, {255, 100, 220});
        }
    }
    if (modelVertices) for (const auto& pair : recipe.fitPairs) {
        if (pair.vertexId >= modelVertices->size() / 3) continue;
        const auto offset = static_cast<std::size_t>(pair.vertexId) * 3;
        const Vec source((*modelVertices)[offset], (*modelVertices)[offset+1], (*modelVertices)[offset+2]);
        const auto target = Transform(inverse, Vector(pair.nominalPoint));
        // 标记输入对应的位置，不添加新的拟合值或把每个对应画成坐标十字。
        const auto marker = points->InsertNextPoint(source.val);
        markers->InsertNextCell(1, &marker);
        const unsigned char color[]{200,180,70}; markerColors->InsertNextTypedTuple(color);
        if ((source-target).dot(source-target) > 0) add(source,target,{255,100,220});
    }
    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetVerts(markers);
    data->SetLines(lines);
    // VTK cell 顺序为 verts 在前、lines 在后，保持每类几何颜色对应。
    for (vtkIdType index = 0; index < colors->GetNumberOfTuples(); ++index) {
        unsigned char color[3]; colors->GetTypedTuple(index, color);
        markerColors->InsertNextTypedTuple(color);
    }
    data->GetCellData()->SetScalars(markerColors);
    return data;
}
