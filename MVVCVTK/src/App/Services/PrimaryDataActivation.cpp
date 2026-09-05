#include "App/Services/PrimaryDataActivation.h"
#include "App/AppState.h"
#include "Data/DataPayloads.h"
#include "Geometry/InteractionComputeService.h"
#include <vtkImageData.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

PrimaryDataActivation::PrimaryDataActivation(
    AbstractDataManager& data, SharedInteractionState& state)
    : m_data(data), m_state(state)
{
}

DataCommitResult PrimaryDataActivation::SetDataCommit(DataTransaction transaction)
{
    const bool hasPrimary = std::any_of(transaction.bindings.begin(),
        transaction.bindings.end(), [](const DataBindingUpdate& update) {
            return update.binding == primaryVolumeBinding;
        });
    auto result = m_data.SetDataCommit(std::move(transaction));
    if (hasPrimary && result.isActivated
        && result.status != DataCommitStatus::Rejected) {
        SetReady(std::nullopt, false);
    }
    return result;
}

void PrimaryDataActivation::SetLoadReady(const std::optional<DataReadyState>& staged)
{
    SetReady(staged, true);
}

void PrimaryDataActivation::SetReady(
    const std::optional<DataReadyState>& staged, const bool isLoad)
{
    // DataGraph 的观察者可能重入换绑；只投影发布之后的真实 current。
    const auto current = m_data.GetPrimaryImage();
    DataReadyState ready;
    if (!isLoad) {
        const auto* payload = current && current->data
            ? dynamic_cast<const ImageGrid3DPayload*>(current->data->payload.get())
            : nullptr;
        if (!current || !current->binding || !payload) return;
        ready.dataRevision = current->data->self;
        ready.bindingRevision = current->binding->revision;
        ready.scalarRange = payload->GetScalarRange();
        ready.spacing = payload->GetGeometry().spacing;
        ready.cursorWorld = m_state.GetCursorWorld();
    }
    else if (staged && current && current->data && current->binding
        && staged->dataRevision == current->data->self
        && staged->bindingRevision == current->binding->revision) {
        ready = *staged;
    }
    else if (!GetDataReadyState(current, &m_state, ready)) {
        // 已发布的 CAS 不可反报失败；损坏输入沿用原有最小终态投影。
        ready.dataRevision = current && current->data
            ? current->data->self : DataRevisionRef{};
        ready.bindingRevision = m_data.GetPrimaryBindingRevision();
        ready.scalarRange = m_data.GetScalarRange();
        ready.spacing = m_data.GetSpacing();
        ready.cursorWorld = m_state.GetCursorWorld();
    }
    if (isLoad) m_state.SetDataReady(ready);
    else m_state.SetImageDataReady(ready);
}

bool PrimaryDataActivation::GetDataReadyState(
    const VtkImageGridSnapshot& snapshot,
    const SharedInteractionState* sharedState,
    DataReadyState& state)
{
    if (!snapshot || !snapshot->image || !snapshot->data
        || !snapshot->binding || snapshot->binding->revision == 0) {
        return false;
    }

    double imageRange[2] = {};
    double imageSpacing[3] = {};
    double imageCenter[3] = {};
    snapshot->image->GetScalarRange(imageRange);
    snapshot->image->GetSpacing(imageSpacing);
    snapshot->image->GetCenter(imageCenter);

    double centerWorld[3] = {};
    auto modelToWorld = vtkSmartPointer<vtkMatrix4x4>::New();
    modelToWorld->Identity();
    if (sharedState) {
        const auto matrix = sharedState->GetModelMatrix();
        modelToWorld->DeepCopy(matrix.data());
    }
    InteractionComputeService::GetWorldPositionFromModel(
        modelToWorld, imageCenter, centerWorld);
    const auto isFinite = [](const double value) {
        return std::isfinite(value);
    };
    if (!std::all_of(std::begin(imageRange), std::end(imageRange), isFinite)
        || !std::all_of(std::begin(imageSpacing), std::end(imageSpacing), isFinite)
        || !std::all_of(std::begin(centerWorld), std::end(centerWorld), isFinite)) {
        return false;
    }

    state.dataRevision = snapshot->data->self;
    state.bindingRevision = snapshot->binding->revision;
    std::copy_n(imageRange, state.scalarRange.size(), state.scalarRange.begin());
    std::copy_n(imageSpacing, state.spacing.size(), state.spacing.begin());
    std::copy_n(centerWorld, state.cursorWorld.size(), state.cursorWorld.begin());
    return true;
}
