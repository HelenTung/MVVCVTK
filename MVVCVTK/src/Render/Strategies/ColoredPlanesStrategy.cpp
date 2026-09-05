#include "ColoredPlanesStrategy.h"

#include <vtkImageData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>

#include <algorithm>
#include <cmath>
#include <limits>

bool ColoredPlanesStrategy::BuildWorldBounds(
    const double bounds[6],
    const std::array<double, 16>& modelMatrix,
    std::array<double, 6>& worldBounds)
{
    if (!bounds || !std::all_of(
            modelMatrix.begin(), modelMatrix.end(),
            [](const double value) { return std::isfinite(value); })) {
        return false;
    }
    worldBounds[0] = worldBounds[2] = worldBounds[4] =
        (std::numeric_limits<double>::max)();
    worldBounds[1] = worldBounds[3] = worldBounds[5] =
        (std::numeric_limits<double>::lowest)();
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                const std::array<double, 4> input{
                    ix == 0 ? bounds[0] : bounds[1],
                    iy == 0 ? bounds[2] : bounds[3],
                    iz == 0 ? bounds[4] : bounds[5],
                    1.0
                };
                std::array<double, 4> output{};
                for (std::size_t row = 0; row < 4; ++row) {
                    for (std::size_t column = 0; column < 4; ++column) {
                        output[row] += modelMatrix[row * 4 + column]
                            * input[column];
                    }
                }
                const double inverseW = std::abs(output[3]) > 1e-12
                    ? 1.0 / output[3] : 1.0;
                const double x = output[0] * inverseW;
                const double y = output[1] * inverseW;
                const double z = output[2] * inverseW;
                if (!std::isfinite(x) || !std::isfinite(y)
                    || !std::isfinite(z)) {
                    return false;
                }
                worldBounds[0] = std::min(worldBounds[0], x);
                worldBounds[1] = std::max(worldBounds[1], x);
                worldBounds[2] = std::min(worldBounds[2], y);
                worldBounds[3] = std::max(worldBounds[3], y);
                worldBounds[4] = std::min(worldBounds[4], z);
                worldBounds[5] = std::max(worldBounds[5], z);
            }
        }
    }
    return true;
}

ColoredPlanesStrategy::ColoredPlanesStrategy()
{
    constexpr double colors[3][3] = {
        { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 1.0 }
    };
    for (int index = 0; index < 3; ++index) {
        m_planeSources[index] = vtkSmartPointer<vtkPlaneSource>::New();
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(
            m_planeSources[index]->GetOutputPort());
        m_planeActors[index] = vtkSmartPointer<vtkActor>::New();
        m_planeActors[index]->SetMapper(mapper);
        m_planeActors[index]->GetProperty()->SetColor(
            colors[index][0], colors[index][1], colors[index][2]);
        m_planeActors[index]->GetProperty()->SetOpacity(0.2);
        m_planeActors[index]->GetProperty()->SetLighting(false);
        AttachProp(m_planeActors[index]);
    }
    m_planeSources[0]->SetNormal(1.0, 0.0, 0.0);
    m_planeSources[1]->SetNormal(0.0, 1.0, 0.0);
    m_planeSources[2]->SetNormal(0.0, 0.0, 1.0);
}

void ColoredPlanesStrategy::SetInputData(
    vtkSmartPointer<vtkDataObject> data)
{
    auto* image = vtkImageData::SafeDownCast(data);
    if (!image) return;
    const vtkMTimeType geometryMTime = image->GetMTime();
    if (m_lastInput == data
        && m_inputGeometryMTime == geometryMTime) {
        return;
    }
    m_lastInput = std::move(data);
    m_inputGeometryMTime = geometryMTime;
    image->GetBounds(m_bounds);
    int dimensions[3]{};
    image->GetDimensions(dimensions);
    for (int axis = 0; axis < 3; ++axis) {
        m_maxIndices[axis] = dimensions[axis] - 1;
    }
    image->GetOrigin(m_origin);
    image->GetSpacing(m_spacing);
    m_hasTransformCache = false;

    m_planeSources[0]->SetOrigin(
        m_origin[0], m_bounds[2], m_bounds[4]);
    m_planeSources[0]->SetPoint1(
        m_origin[0], m_bounds[3], m_bounds[4]);
    m_planeSources[0]->SetPoint2(
        m_origin[0], m_bounds[2], m_bounds[5]);
    m_planeSources[1]->SetOrigin(
        m_bounds[0], m_origin[1], m_bounds[4]);
    m_planeSources[1]->SetPoint1(
        m_bounds[1], m_origin[1], m_bounds[4]);
    m_planeSources[1]->SetPoint2(
        m_bounds[0], m_origin[1], m_bounds[5]);
    m_planeSources[2]->SetOrigin(
        m_bounds[0], m_bounds[2], m_origin[2]);
    m_planeSources[2]->SetPoint1(
        m_bounds[1], m_bounds[2], m_origin[2]);
    m_planeSources[2]->SetPoint2(
        m_bounds[0], m_bounds[3], m_origin[2]);
    for (auto& actor : m_planeActors) {
        actor->SetPosition(0.0, 0.0, 0.0);
    }
}

std::optional<ColoredPlaneStage>
ColoredPlanesStrategy::BuildVisualStage(
    const RenderParams& params,
    const UpdateFlags flags) const
{
    ColoredPlaneStage stage;
    stage.modelMatrix = params.modelMatrix;
    stage.inputGeometryMTime = m_inputGeometryMTime;
    stage.hasVisibility =
        (flags & UpdateFlags::Visibility) != UpdateFlags::None;
    stage.visibility =
        (params.visibilityMask & VisFlags::Planes3D) ? 1 : 0;

    const bool hasPositionUpdate =
        (flags & UpdateFlags::Cursor) != UpdateFlags::None
        || (flags & UpdateFlags::Transform) != UpdateFlags::None;
    if (!hasPositionUpdate) return stage;
    auto* image = vtkImageData::SafeDownCast(m_lastInput);
    if (!image) return std::nullopt;

    const vtkMTimeType inputGeometryMTime = image->GetMTime();
    const bool hasInputGeometryChange =
        m_inputGeometryMTime != inputGeometryMTime;
    stage.inputGeometryMTime = inputGeometryMTime;
    if (hasInputGeometryChange) {
        image->GetBounds(stage.inputBounds.data());
        stage.hasInputGeometrySnapshot = true;
    }
    else {
        std::copy(std::begin(m_bounds), std::end(m_bounds),
            stage.inputBounds.begin());
    }
    const bool needsTransformCache = !m_hasTransformCache
        || m_modelMatrix != params.modelMatrix
        || hasInputGeometryChange;
    if (needsTransformCache) {
        if (!BuildWorldBounds(
                stage.inputBounds.data(), params.modelMatrix,
                stage.worldBounds)) {
            return std::nullopt;
        }
        stage.hasTransformCache = true;
    }
    else {
        stage.worldBounds = m_worldBounds;
    }
    const auto& bounds = stage.worldBounds;
    const double x = std::clamp(
        params.cursor[0], bounds[0], bounds[1]);
    const double y = std::clamp(
        params.cursor[1], bounds[2], bounds[3]);
    const double z = std::clamp(
        params.cursor[2], bounds[4], bounds[5]);
    stage.origins[0] = { x, bounds[2], bounds[4] };
    stage.point1[0] = { x, bounds[3], bounds[4] };
    stage.point2[0] = { x, bounds[2], bounds[5] };
    stage.origins[1] = { bounds[0], y, bounds[4] };
    stage.point1[1] = { bounds[1], y, bounds[4] };
    stage.point2[1] = { bounds[0], y, bounds[5] };
    stage.origins[2] = { bounds[0], bounds[2], z };
    stage.point1[2] = { bounds[1], bounds[2], z };
    stage.point2[2] = { bounds[0], bounds[3], z };
    stage.hasGeometry = true;
    return stage;
}

bool ColoredPlanesStrategy::SetVisualCommit(
    const ColoredPlaneStage& stage)
{
    if (stage.hasGeometry) {
        for (std::size_t index = 0; index < 3; ++index) {
            m_planeSources[index]->SetOrigin(
                stage.origins[index][0],
                stage.origins[index][1],
                stage.origins[index][2]);
            m_planeSources[index]->SetPoint1(
                stage.point1[index][0],
                stage.point1[index][1],
                stage.point1[index][2]);
            m_planeSources[index]->SetPoint2(
                stage.point2[index][0],
                stage.point2[index][1],
                stage.point2[index][2]);
            m_planeActors[index]->SetUserMatrix(nullptr);
        }
    }
    if (stage.hasTransformCache) {
        if (stage.hasInputGeometrySnapshot) {
            std::copy(stage.inputBounds.begin(), stage.inputBounds.end(),
                std::begin(m_bounds));
        }
        m_worldBounds = stage.worldBounds;
        m_modelMatrix = stage.modelMatrix;
        m_inputGeometryMTime = stage.inputGeometryMTime;
        m_hasTransformCache = true;
        ++m_worldBoundsBuildCount;
    }
    if (stage.hasVisibility) {
        for (auto& actor : m_planeActors) {
            actor->SetVisibility(stage.visibility);
        }
    }
    return true;
}

bool ColoredPlanesStrategy::SetVisualState(
    const RenderParams& params,
    const UpdateFlags flags)
{
    const auto stage = BuildVisualStage(params, flags);
    return stage && SetVisualCommit(*stage);
}

int ColoredPlanesStrategy::GetPlaneAxis(vtkActor* actor)
{
    for (int index = 0; index < 3; ++index) {
        if (m_planeActors[index] == actor) return index;
    }
    return -1;
}
