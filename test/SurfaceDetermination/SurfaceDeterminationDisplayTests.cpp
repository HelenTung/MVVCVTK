#include "SurfaceDeterminationTestCases.h"

#include "SurfaceDeterminationTestSupport.h"
#include "SurfaceOverlayStrategy.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkMapper.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace {

using namespace SurfaceTest;

vtkSmartPointer<vtkPolyData> BuildCube()
{
    constexpr std::array<std::array<double, 3>, 8> positions{{
        {{ -1.0, -1.0, -1.0 }},
        {{ 1.0, -1.0, -1.0 }},
        {{ 1.0, 1.0, -1.0 }},
        {{ -1.0, 1.0, -1.0 }},
        {{ -1.0, -1.0, 1.0 }},
        {{ 1.0, -1.0, 1.0 }},
        {{ 1.0, 1.0, 1.0 }},
        {{ -1.0, 1.0, 1.0 }}
    }};
    constexpr std::array<std::array<vtkIdType, 3>, 12> triangles{{
        {{ 0, 2, 1 }}, {{ 0, 3, 2 }},
        {{ 4, 5, 6 }}, {{ 4, 6, 7 }},
        {{ 0, 1, 5 }}, {{ 0, 5, 4 }},
        {{ 1, 2, 6 }}, {{ 1, 6, 5 }},
        {{ 2, 3, 7 }}, {{ 2, 7, 6 }},
        {{ 3, 0, 4 }}, {{ 3, 4, 7 }}
    }};
    auto points = vtkSmartPointer<vtkPoints>::New();
    for (const auto& point : positions) points->InsertNextPoint(point.data());
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    for (const auto& triangle : triangles) {
        cells->InsertNextCell(3, triangle.data());
    }
    auto surface = vtkSmartPointer<vtkPolyData>::New();
    surface->SetPoints(points);
    surface->SetPolys(cells);
    return surface;
}

vtkActor* GetOnlyActor(vtkRenderer& renderer)
{
    auto* props = renderer.GetViewProps();
    if (!props || props->GetNumberOfItems() != 1) return nullptr;
    props->InitTraversal();
    return vtkActor::SafeDownCast(props->GetNextProp());
}

void TestSurfaceOverlay(Checks& checks)
{
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto overlay = std::make_shared<SurfaceOverlayStrategy>();
    overlay->SetInputData(BuildCube());
    overlay->AttachRenderer(renderer);
    auto* actor = GetOnlyActor(*renderer);
    checks.Get(actor != nullptr, "3D overlay attaches exactly one actor");
    if (actor) {
        auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        mapper->Update();
        checks.Get(
            mapper->GetInput()
                && mapper->GetInput()->GetNumberOfPolys() == 12,
            "3D overlay preserves display triangle count");
    }

    FeatureOverlayState state;
    state.modelToWorld = {
        0.0, -2.0, 0.0, 4.0,
        3.0, 0.0, 0.0, -2.0,
        0.0, 0.0, 0.5, 3.0,
        0.0, 0.0, 0.0, 1.0
    };
    overlay->SetOverlayState(state);
    actor = GetOnlyActor(*renderer);
    bool matrixMatches = actor && actor->GetUserMatrix();
    for (std::size_t row = 0; matrixMatches && row < 4; ++row) {
        for (std::size_t column = 0;
            matrixMatches && column < 4; ++column) {
            matrixMatches = actor->GetUserMatrix()->GetElement(
                static_cast<int>(row), static_cast<int>(column))
                == state.modelToWorld[row * 4 + column];
        }
    }
    checks.Get(
        matrixMatches,
        "3D overlay applies the full affine model-to-world matrix");
    double modelPoint[4]{ -1.0, -1.0, -1.0, 1.0 };
    double worldPoint[4]{};
    if (actor) actor->GetMatrix()->MultiplyPoint(modelPoint, worldPoint);
    checks.Get(
        actor
            && std::abs(worldPoint[0] - 6.0) < 1.0e-12
            && std::abs(worldPoint[1] + 5.0) < 1.0e-12
            && std::abs(worldPoint[2] - 2.5) < 1.0e-12
            && std::abs(worldPoint[3] - 1.0) < 1.0e-12,
        "3D actor transforms model geometry to the expected world coordinate");

    overlay->DetachRenderer(renderer);
    checks.Get(
        renderer->GetViewProps()->GetNumberOfItems() == 0,
        "3D overlay detaches all owned props");
}

void TestSliceOverlay(Checks& checks)
{
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto overlay = std::make_shared<SurfaceSliceOverlayStrategy>(
        std::array<double, 3>{ 1.0, 0.0, 0.0 });
    overlay->SetInputData(BuildCube());
    FeatureOverlayState state;
    state.cursor = { 0.25, 0.0, 0.0 };
    state.modelToWorld[11] = 3.0;
    overlay->SetOverlayState(state);
    overlay->AttachRenderer(renderer);
    auto* actor = GetOnlyActor(*renderer);
    checks.Get(actor != nullptr, "slice overlay attaches exactly one actor");
    if (actor) {
        auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        mapper->Update();
        checks.Get(
            mapper->GetInput()
                && mapper->GetInput()->GetNumberOfCells() > 0,
            "slice overlay cuts a model-space surface");
        checks.Get(
            actor->GetUserMatrix()
                && actor->GetUserMatrix()->GetElement(2, 3) == 3.0,
            "slice overlay applies model-to-world transform");
    }

    state.cursor = { 2.0, 0.0, 0.0 };
    overlay->SetOverlayState(state);
    if (actor) {
        auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        mapper->Update();
        checks.Get(
            mapper->GetInput()
                && mapper->GetInput()->GetNumberOfCells() == 0,
            "slice cursor update moves the cutter plane");
    }
    overlay->DetachRenderer(renderer);
    checks.Get(
        renderer->GetViewProps()->GetNumberOfItems() == 0,
        "slice overlay detaches all owned props");
}

void TestRotatedSliceNormal(Checks& checks)
{
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto overlay = std::make_shared<SurfaceSliceOverlayStrategy>(
        std::array<double, 3>{ 1.0, 1.0, 0.0 });
    overlay->SetInputData(BuildCube());
    FeatureOverlayState state;
    state.cursor = { 0.25, 0.25, 0.0 };
    overlay->SetOverlayState(state);
    overlay->AttachRenderer(renderer);
    auto* actor = GetOnlyActor(*renderer);
    auto* mapper = actor
        ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper()) : nullptr;
    if (mapper) mapper->Update();
    checks.Get(
        mapper && mapper->GetInput()
            && mapper->GetInput()->GetNumberOfCells() > 0,
        "slice overlay honors a non-axis direction normal");
    overlay->DetachRenderer(renderer);
}

} // namespace

int GetSurfaceDisplayFailCount()
{
    Checks checks;
    TestSurfaceOverlay(checks);
    TestSliceOverlay(checks);
    TestRotatedSliceNormal(checks);
    return checks.failureCount;
}
