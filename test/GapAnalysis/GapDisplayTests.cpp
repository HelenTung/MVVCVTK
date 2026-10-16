// 测试用途：验证孔隙结果显示、视图切换、退出清理、失败重试和回调边界。
#include "GapDisplayTests.h"
#include "../TestDataPort.h"
#include "Host/GapHostFeature.h"

#include "Services/GapAnalysisService.h"
#include "Render/GapDisplayData.h"
#include "Geometry/RoiEvaluator.h"
#include "Render/Contracts/FeatureOverlay.h"
#include "Render/Contracts/OverlayService.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool GetArraySame(vtkDataArray* first, vtkDataArray* second)
{
    return first && second && first->GetDataType() == second->GetDataType()
        && first->GetNumberOfComponents() == second->GetNumberOfComponents()
        && first->GetNumberOfValues() == second->GetNumberOfValues()
        && std::memcmp(first->GetVoidPointer(0), second->GetVoidPointer(0),
            static_cast<std::size_t>(first->GetNumberOfValues()) * first->GetDataTypeSize()) == 0;
}

static_assert(noexcept(
    std::declval<OverlayService&>().RemoveOverlay(
        std::declval<std::shared_ptr<FeatureOverlay>>())),
    "Overlay removal must not reopen the StartView exception boundary.");

class OverlayStub final : public OverlayService {
public:
    bool AttachOverlay(
        std::shared_ptr<FeatureOverlay> overlay) override
    {
        if (!overlay) {
            return false;
        }
        m_overlay = std::move(overlay);
        m_overlays.push_back(m_overlay);
        m_overlay->AttachRenderer(m_renderer);
        m_inputs.push_back(GetAttachedInput());
        ++m_attachCount;
        return !isAttachRejected;
    }

    void RemoveOverlay(
        std::shared_ptr<FeatureOverlay> overlay) noexcept override
    {
        const auto found = std::find(m_overlays.begin(), m_overlays.end(), overlay);
        if (found == m_overlays.end()) {
            return;
        }
        overlay->DetachRenderer(m_renderer);
        m_overlays.erase(found);
        m_overlay = m_overlays.empty() ? nullptr : m_overlays.back();
        ++m_removeCount;
    }

    void ClearOverlays() noexcept override
    {
        for (const auto& overlay : m_overlays) overlay->DetachRenderer(m_renderer);
        m_overlays.clear();
        m_overlay.reset();
    }

    int GetAttachCount() const { return m_attachCount; }
    int GetRemoveCount() const { return m_removeCount; }
    int GetPropCount() const { return m_renderer->GetViewProps()->GetNumberOfItems(); }
    bool isAttachRejected = false;
    std::shared_ptr<FeatureOverlay> GetOverlay() const { return m_overlay; }
    vtkDataObject* GetInput(const std::size_t index) const
    {
        return index < m_inputs.size() ? m_inputs[index] : nullptr;
    }

private:
    vtkDataObject* GetAttachedInput() const
    {
        auto* props = m_renderer->GetViewProps();
        if (!props) {
            return nullptr;
        }
        props->InitTraversal();
        while (auto* nextProp = props->GetNextProp()) {
        if (auto* slice = vtkImageSlice::SafeDownCast(nextProp)) {
            auto* mapper = vtkImageResliceMapper::SafeDownCast(
                slice->GetMapper());
            return mapper ? mapper->GetInput() : nullptr;
        }
        if (auto* actor = vtkActor::SafeDownCast(nextProp)) {
            auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
            if (mapper && mapper->GetScalarVisibility()) return mapper->GetInput();
        }
        }
        return nullptr;
    }

    std::shared_ptr<FeatureOverlay> m_overlay;
    std::vector<std::shared_ptr<FeatureOverlay>> m_overlays;
    vtkSmartPointer<vtkRenderer> m_renderer =
        vtkSmartPointer<vtkRenderer>::New();
    std::vector<vtkDataObject*> m_inputs;
    int m_attachCount = 0;
    int m_removeCount = 0;
};

vtkSmartPointer<vtkImageData> GetMask(
    vtkImageData* image,
    unsigned char value = 255)
{
    if (!image) {
        return nullptr;
    }
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* values = static_cast<unsigned char*>(
        mask->GetScalarPointer());
    if (!values) {
        return nullptr;
    }
    std::fill_n(values, mask->GetNumberOfPoints(), value);
    mask->Modified();
    return mask;
}

VtkImageGridSnapshot BuildGraphInput(
    const vtkSmartPointer<vtkImageData>& image,
    vtkSmartPointer<vtkImageData> validityMask = nullptr)
{
    auto data = std::make_shared<TestDataPort>();
    return data->SetPrimaryImage(image, validityMask);
}

bool CommitDisplayResult(GapAnalysisService& service,
    std::shared_ptr<const GapDisplayData>* completedDisplay = nullptr)
{
    GapAnalysisResult candidate;
    if (!service.GetCompletedResult(candidate)) return false;
    if (completedDisplay) *completedDisplay = candidate.display;
    auto labels = candidate.payloads ? candidate.payloads->labels : nullptr;
    auto mesh = candidate.payloads ? candidate.payloads->mesh : nullptr;
    if (!candidate.payloads) {
        // The legacy raw-input service has no graph publication payloads.
        // Project its surface geometry, matching Gap's formal mesh contract;
        // contour display arrays are not published Gap mesh attributes.
        VtkDataBridge bridge;
        labels = bridge.CreateLabelPayload(candidate.labelImage);
        if (!candidate.voidMesh) return false;
        auto geometry = vtkSmartPointer<vtkPolyData>::New();
        geometry->SetPoints(candidate.voidMesh->GetPoints());
        geometry->SetPolys(candidate.voidMesh->GetPolys());
        mesh = bridge.CreateMeshPayload(geometry);
    }
    TestDataPort committedData;
    auto views = committedData.SetLabelAndMesh(std::move(labels), std::move(mesh));
    const auto displayResult=views.second ? views.second->data->self:DataRevisionRef{};
    return views.first && views.second
        && service.SetCommittedView(
            std::move(views.first), std::move(views.second),displayResult);
}

bool StartDisplay(
    GapAnalysisService& service,
    GapViewRequest request,
    vtkImageData* image,
    std::shared_ptr<const GapDisplayData>* completedDisplay = nullptr)
{
    bool isCompleted = false;
    bool isSucceeded = false;
    if (!service.StartView(
            std::move(request),
            [&](const bool isSuccess) {
                isCompleted = true;
                isSucceeded = isSuccess;
            })) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (service.GetAnalysisState() == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.OnDisplayTick(image);
    if (service.GetAnalysisState() == GapAnalysisState::Succeeded) {
        (void)CommitDisplayResult(service, completedDisplay);
    }
    return service.GetAnalysisState() == GapAnalysisState::Succeeded
        && isCompleted && isSucceeded;
}

}

int GapDisplaySuite::GetFailCount() const
{
    int failureCount = 0;
    const auto expect = [&failureCount](bool isExpected, const char* message) {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    };

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(5, 5, 5);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* voxels = static_cast<float*>(image->GetScalarPointer());
    std::fill_n(voxels, 125, 100.0f);
    voxels[2 + 5 * (2 + 5 * 2)] = 0.0f;

    GapSurfaceConfig surfaceConfig;
    surfaceConfig.isoMode = GapIsoMode::AbsoluteValue;
    surfaceConfig.absoluteIsoValue = 50.0;
    surfaceConfig.backgroundMean = 0.0f;
    surfaceConfig.materialMean = 100.0f;
    GapVoidParams voidParams;
    voidParams.isFilterEnabled = false;
    voidParams.minVolumeMM3 = 0.0;

    auto overlay = std::make_shared<OverlayStub>();
    auto meshOverlay = std::make_shared<OverlayStub>();
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceTargets;
    sliceTargets.emplace_back(Orientation::Top_down, overlay);
    GapAnalysisService service;
    GapViewRequest viewRequest;
    viewRequest.inputImage = image;
    viewRequest.surface = surfaceConfig;
    viewRequest.voidParams = voidParams;
    viewRequest.meshTargets.push_back(meshOverlay);
    viewRequest.sliceTargets = sliceTargets;
    bool isCompleted = false;
    bool isCompletionOk = false;
    expect(service.StartView(std::move(viewRequest),
        [&](bool isSuccess) {
            isCompleted = true;
            isCompletionOk = isSuccess;
        }), "Gap view should accept mesh and slice targets.");
    expect(service.GetAnalysisState() != GapAnalysisState::Idle,
        "Accepted Gap view should reserve and start its worker before returning.");
    service.OnDisplayTick(nullptr);

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (service.GetAnalysisState() == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(service.GetAnalysisState() == GapAnalysisState::Succeeded,
        "Gap display worker should succeed.");
    expect(service.GetLabelStorageTransferred(),
        "Gap result should retain the kernel label owner without a second full-volume copy.");

    service.OnDisplayTick(image);
    expect(CommitDisplayResult(service),
        "Committed Gap views should be accepted before display attach.");
    auto* firstSliceInput = overlay->GetInput(0);
    auto* firstMeshInput = meshOverlay->GetInput(0);
    expect(overlay->GetAttachCount() == 1
            && meshOverlay->GetAttachCount() == 1
            && overlay->GetOverlay()
            && meshOverlay->GetOverlay()
            && vtkImageData::SafeDownCast(firstSliceInput)
            && vtkPolyData::SafeDownCast(firstMeshInput)
            && isCompleted && isCompletionOk,
        "Terminal tick should attach one mesh and one slice overlay before completing.");
    expect(service.SwitchOverlay()
            && overlay->GetRemoveCount() == 1
            && meshOverlay->GetRemoveCount() == 1,
        "Hide should detach both overlays without discarding the result.");
    expect(service.SwitchOverlay()
            && overlay->GetAttachCount() == 2
            && meshOverlay->GetAttachCount() == 2
            && overlay->GetInput(1) == firstSliceInput
            && meshOverlay->GetInput(1) == firstMeshInput,
        "Show should reuse both stored display artifacts.");

    {
        auto paletteSlice = std::make_shared<OverlayStub>();
        auto paletteMesh = std::make_shared<OverlayStub>();
        GapAnalysisService paletteService;
        GapViewRequest paletteRequest;
        paletteRequest.inputImage = image; paletteRequest.surface = surfaceConfig;
        paletteRequest.voidParams = voidParams; paletteRequest.meshTargets = {paletteMesh};
        paletteRequest.sliceTargets = {{Orientation::Top_down, paletteSlice}};
        const bool isReady = StartDisplay(paletteService, std::move(paletteRequest), image);
        expect(isReady, "Independent palette fixture should complete its own analysis.");
        if (isReady) {
            const auto storedLabels = paletteService.BuildLabelImage();
            const auto storedMesh = paletteService.BuildVoidMesh();
            const auto storedStats = paletteService.GetStatistics();
            const auto* storedValues = static_cast<const int*>(storedLabels->GetScalarPointer());
            const std::vector<int> labelValues(storedValues, storedValues + storedLabels->GetNumberOfPoints());
            GapDisplayParams colors;
            std::vector<GapDisplayParams> displays;
            for (const auto mode : {GapColorMode::Constant, GapColorMode::Gradient, GapColorMode::Rainbow,
                     GapColorMode::InverseRainbow, GapColorMode::HueLoop}) {
                GapDisplayParams next;next.mode=mode;displays.push_back(next);
            }
            for(const auto style:{GapDisplayStyle::Inclined,GapDisplayStyle::InverseInclined}) {
                GapDisplayParams next;next.style=style;
                next.segments={{std::nullopt,1,GapColorMode::Constant,{.6,.2,.7},{.6,.2,.7}},
                    {1,2,GapColorMode::Gradient,{.2,.4,.7},{.7,.4,.2}},
                    {2,std::nullopt,GapColorMode::Constant,{.2,.3,.8},{.2,.3,.8}}};
                displays.push_back(next);
            }
            for (const auto& next:displays) {
                colors=next;
                expect(paletteService.SetDisplay(colors) && paletteService.GetDisplayParams().mode == next.mode
                    && paletteService.GetDisplayParams().style==next.style,
                    "Every color mode should replace the display on the owner thread.");
                const auto statistics = paletteService.GetStatistics();
                const auto labels = paletteService.BuildLabelImage();
                const auto mesh = paletteService.BuildVoidMesh();
                expect(labels && mesh && labels->GetNumberOfPoints() == storedLabels->GetNumberOfPoints()
                    && GetArraySame(labels->GetPointData()->GetScalars(), storedLabels->GetPointData()->GetScalars())
                    && std::equal(labelValues.begin(), labelValues.end(), static_cast<const int*>(labels->GetScalarPointer()))
                    && GetArraySame(mesh->GetPoints()->GetData(), storedMesh->GetPoints()->GetData())
                    && GetArraySame(mesh->GetPolys()->GetOffsetsArray(), storedMesh->GetPolys()->GetOffsetsArray())
                    && GetArraySame(mesh->GetPolys()->GetConnectivityArray(), storedMesh->GetPolys()->GetConnectivityArray())
                    && statistics.objectVoxelCount == storedStats.objectVoxelCount
                    && statistics.voidVoxelCount == storedStats.voidVoxelCount
                    && statistics.objectVolumeMM3 == storedStats.objectVolumeMM3
                    && statistics.voidVolumeMM3 == storedStats.voidVolumeMM3
                    && statistics.porosityRatio == storedStats.porosityRatio,
                    "Color switching must preserve exact supplier labels, mesh and statistics.");
            }
            colors={};colors.mode=GapColorMode::HueLoop;
            expect(paletteService.SetDisplay(colors),"Restore the fixed-alpha palette before transactional failure checks.");
            const auto previousSlice = paletteSlice->GetOverlay(), previousMesh = paletteMesh->GetOverlay();
            const int sliceProps = paletteSlice->GetPropCount(), meshProps = paletteMesh->GetPropCount();
            auto invalid=displays.back();invalid.segments[1].lower=1.1;
            expect(!paletteService.SetDisplay(invalid) && paletteSlice->GetOverlay()==previousSlice
                && paletteMesh->GetOverlay()==previousMesh,
                "An invalid segment gap preserves both views and the prior display.");
            colors.mode = GapColorMode::Gradient;
            colors.style=GapDisplayStyle::Inclined;colors.segments=displays.back().segments;
            paletteSlice->isAttachRejected = true;
            expect(!paletteService.SetDisplay(colors) && paletteSlice->GetOverlay() == previousSlice
                && paletteMesh->GetOverlay() == previousMesh
                && paletteSlice->GetPropCount() == sliceProps && paletteMesh->GetPropCount() == meshProps
                && paletteService.GetDisplayParams().mode == GapColorMode::HueLoop,
                "Partially attached palette candidates must roll back without losing the prior display.");
            paletteSlice->isAttachRejected = false;
            expect(!paletteService.SetDisplay(colors, [] { return false; })
                && paletteSlice->GetOverlay() == previousSlice && paletteMesh->GetOverlay() == previousMesh,
                "Rejected scene update must retain the prior palette and props.");
            bool isWrongThreadAccepted = true;
            std::thread paletteThread([&] { isWrongThreadAccepted = paletteService.SetDisplay(colors); });
            paletteThread.join();
            expect(!isWrongThreadAccepted, "Color switching must reject a non-owner thread.");
            expect(paletteService.SwitchOverlay(), "Palette test should hide the display.");
            const int hiddenAttachCount = paletteSlice->GetAttachCount();
            expect(paletteService.SetDisplay(colors) && paletteSlice->GetAttachCount() == hiddenAttachCount
                && !paletteService.GetDisplayOn(), "Hidden palette changes must preserve the hidden state.");
            expect(paletteService.SwitchOverlay() && paletteService.GetDisplayParams().mode == GapColorMode::Gradient,
                "Show must use the palette selected while hidden.");

        }
    }

    GapAnalysisService invalidMaskService;
    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(4, 5, 5);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    GapViewRequest invalidRequest;
    invalidRequest.inputImage = image;
    invalidRequest.validityMask = invalidMask;
    invalidRequest.surface = surfaceConfig;
    invalidRequest.voidParams = voidParams;
    invalidRequest.sliceTargets = sliceTargets;
    expect(!invalidMaskService.StartView(
            std::move(invalidRequest)),
        "Gap view should reject a mask with mismatched geometry.");

    auto invalidTypeMask = vtkSmartPointer<vtkImageData>::New();
    invalidTypeMask->CopyStructure(image);
    invalidTypeMask->AllocateScalars(VTK_FLOAT, 1);
    GapViewRequest invalidTypeRequest;
    invalidTypeRequest.inputImage = image;
    invalidTypeRequest.validityMask = invalidTypeMask;
    invalidTypeRequest.surface = surfaceConfig;
    invalidTypeRequest.voidParams = voidParams;
    invalidTypeRequest.sliceTargets = sliceTargets;
    expect(!invalidMaskService.StartView(
            std::move(invalidTypeRequest)),
        "Gap view should reject a non-byte validity mask.");

    auto invalidDirectionMask = GetMask(image);
    auto invalidDirection = vtkSmartPointer<vtkMatrix3x3>::New();
    invalidDirection->Identity();
    invalidDirection->SetElement(0, 0, -1.0);
    invalidDirectionMask->SetDirectionMatrix(invalidDirection);
    GapViewRequest invalidDirectionRequest;
    invalidDirectionRequest.inputImage = image;
    invalidDirectionRequest.validityMask = invalidDirectionMask;
    invalidDirectionRequest.surface = surfaceConfig;
    invalidDirectionRequest.voidParams = voidParams;
    invalidDirectionRequest.sliceTargets = sliceTargets;
    expect(!invalidMaskService.StartView(
            std::move(invalidDirectionRequest)),
        "Gap view should reject a validity mask with mismatched direction.");
    expect(service.GetViewOn()
        && invalidMaskService.GetAnalysisState()
            == GapAnalysisState::Idle
        && overlay->GetAttachCount() == 2
        && overlay->GetRemoveCount() == 1,
        "Rejected masks should not disturb the prior overlay session.");

    bool isWrongSwitchAccepted = true;
    bool isWrongExitAccepted = true;
    std::thread wrongThread([&]() {
        isWrongSwitchAccepted = service.SwitchOverlay();
        service.OnDisplayTick(image);
        isWrongExitAccepted = service.ExitView();
    });
    wrongThread.join();
    bool isWrongThreadViewOn = false;
    std::thread stateThread([&]() {
        isWrongThreadViewOn = service.GetViewOn();
    });
    stateThread.join();
    expect(!isWrongSwitchAccepted && !isWrongExitAccepted
        && isWrongThreadViewOn && service.GetViewOn() && overlay->GetAttachCount() == 2,
        "Non-owner commands must be rejected while state queries still report the active session.");

    auto firstLabelImage = service.BuildLabelImage();
    expect(firstLabelImage && firstLabelImage->GetScalarPointer(),
        "Gap result should expose one label image copy.");
    if (firstLabelImage && firstLabelImage->GetScalarPointer()) {
        static_cast<int*>(firstLabelImage->GetScalarPointer())[0] = 99;
        auto secondLabelImage = service.BuildLabelImage();
        expect(secondLabelImage
            && secondLabelImage != firstLabelImage
            && secondLabelImage->GetScalarPointer() != firstLabelImage->GetScalarPointer()
            && static_cast<int*>(secondLabelImage->GetScalarPointer())[0] != 99,
            "Mutating one public label image must not pollute later reads.");
    }
    auto firstVoidMesh = service.BuildVoidMesh();
    expect(firstVoidMesh && firstVoidMesh->GetNumberOfPoints() > 0,
        "Gap result should expose one non-empty void mesh copy.");
    if (firstVoidMesh && firstVoidMesh->GetNumberOfPoints() > 0
        && firstVoidMesh->GetPoints()) {
        double firstPoint[3] = {};
        firstVoidMesh->GetPoint(0, firstPoint);
        const double changedX = firstPoint[0] + 123.0;
        firstVoidMesh->GetPoints()->SetPoint(
            0, changedX, firstPoint[1], firstPoint[2]);
        firstVoidMesh->Modified();
        auto secondVoidMesh = service.BuildVoidMesh();
        double secondPoint[3] = {};
        if (secondVoidMesh && secondVoidMesh->GetNumberOfPoints() > 0) {
            secondVoidMesh->GetPoint(0, secondPoint);
        }
        expect(secondVoidMesh
                && secondVoidMesh != firstVoidMesh
                && secondVoidMesh->GetPoints()
                    != firstVoidMesh->GetPoints()
                && secondPoint[0] != changedX,
            "Mutating one public void mesh must not pollute later reads.");
    }
    expect(service.ExitView() && !service.GetViewOn(),
        "Exit should end the display session.");
    expect(overlay->GetRemoveCount() == 2
            && meshOverlay->GetRemoveCount() == 2,
        "Exit should remove every attached Gap overlay.");
    service.OnDisplayTick(nullptr);

    auto sliceOnlyOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService sliceOnlyService;
    GapViewRequest sliceOnlyRequest;
    sliceOnlyRequest.inputImage = image;
    sliceOnlyRequest.surface = surfaceConfig;
    sliceOnlyRequest.voidParams = voidParams;
    sliceOnlyRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        sliceOnlyOverlay);
    expect(StartDisplay(
            sliceOnlyService,
            std::move(sliceOnlyRequest),
            image)
            && sliceOnlyOverlay->GetAttachCount() == 1
            && vtkImageData::SafeDownCast(
                sliceOnlyOverlay->GetInput(0)),
        "Slice-only Gap view should display the shared label result.");
    expect(sliceOnlyService.ExitView(),
        "Slice-only Gap view should exit cleanly.");
    sliceOnlyService.OnDisplayTick(nullptr);

    auto trustedOverlay = std::make_shared<OverlayStub>();
    auto trustedMask = GetMask(image);
    static_cast<unsigned char*>(
        trustedMask->GetScalarPointer())[0] = 0;
    trustedMask->Modified();
    auto trustedInput = BuildGraphInput(image, trustedMask);
    auto* trustedScalars = image->GetPointData()->GetScalars();
    const auto trustedVoxelCount = static_cast<std::size_t>(
        image->GetNumberOfPoints());
    const auto* trustedVoxels = static_cast<const float*>(
        image->GetScalarPointer());
    const std::vector<float> trustedValues(
        trustedVoxels,
        trustedVoxels + trustedVoxelCount);
    const int scalarOwners = trustedScalars->GetReferenceCount();
    GapAnalysisService trustedService;
    GapViewRequest trustedRequest;
    trustedRequest.graphInput = trustedInput;
    trustedRequest.surface = surfaceConfig;
    trustedRequest.voidParams = voidParams;
    trustedRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        trustedOverlay);
    expect(StartDisplay(
            trustedService,
            std::move(trustedRequest),
            image)
            && trustedScalars->GetReferenceCount() == scalarOwners,
        "Graph Gap input is isolated from the caller-owned scalar array.");
    expect(trustedService.ExitView(),
        "Trusted Gap view should exit cleanly.");
    trustedService.OnDisplayTick(nullptr);
    expect(trustedScalars->GetReferenceCount() == scalarOwners,
        "Graph Gap input does not retain the caller-owned scalar array.");
    const auto* releasedVoxels = static_cast<const float*>(
        image->GetScalarPointer());
    expect(releasedVoxels
            && std::equal(
                trustedValues.begin(),
                trustedValues.end(),
                releasedVoxels),
        "Graph Gap analysis must not modify caller-owned scalars.");

    GapAnalysisService mixedInputService;
    GapViewRequest mixedInputRequest;
    mixedInputRequest.graphInput = trustedInput;
    mixedInputRequest.inputImage = image;
    mixedInputRequest.surface = surfaceConfig;
    mixedInputRequest.voidParams = voidParams;
    mixedInputRequest.sliceTargets = sliceTargets;
    expect(!mixedInputService.StartView(
            std::move(mixedInputRequest)),
        "Gap view should reject ambiguous graph and mutable inputs.");

    auto meshOnlyOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService meshOnlyService;
    GapViewRequest meshOnlyRequest;
    meshOnlyRequest.inputImage = image;
    meshOnlyRequest.surface = surfaceConfig;
    meshOnlyRequest.voidParams = voidParams;
    meshOnlyRequest.meshTargets.push_back(meshOnlyOverlay);
    expect(StartDisplay(
            meshOnlyService,
            std::move(meshOnlyRequest),
            image)
            && meshOnlyOverlay->GetAttachCount() == 1
            && vtkPolyData::SafeDownCast(
                meshOnlyOverlay->GetInput(0)),
        "Mesh-only Gap view should display the worker-built mesh result.");
    expect(meshOnlyService.ExitView(),
        "Mesh-only Gap view should exit cleanly.");
    meshOnlyService.OnDisplayTick(nullptr);

    GapAnalysisService ownerService;
    auto ownerImage = vtkSmartPointer<vtkImageData>::New();
    ownerImage->DeepCopy(image);
    vtkWeakPointer<vtkImageData> weakOwner;
    weakOwner = ownerImage.GetPointer();
    GapViewRequest ownerRequest;
    ownerRequest.inputImage = ownerImage;
    ownerRequest.surface = surfaceConfig;
    ownerRequest.voidParams = voidParams;
    ownerRequest.sliceTargets = sliceTargets;
    expect(ownerService.StartView(std::move(ownerRequest)),
        "Owner release view should isolate one controlled input snapshot.");
    ownerImage = nullptr;
    expect(weakOwner == nullptr,
        "Gap view isolation should not retain the caller's mutable image.");
    expect(ownerService.ExitView(), "Display exit should be accepted.");
    ownerService.OnDisplayTick(nullptr);
    expect(weakOwner == nullptr,
        "Exit completion should keep the caller image released.");

    auto maskedLowImage = vtkSmartPointer<vtkImageData>::New();
    maskedLowImage->DeepCopy(image);
    auto maskedHighImage = vtkSmartPointer<vtkImageData>::New();
    maskedHighImage->DeepCopy(image);
    auto maskedNanImage = vtkSmartPointer<vtkImageData>::New();
    maskedNanImage->DeepCopy(image);
    static_cast<float*>(maskedLowImage->GetScalarPointer())[0] = -1000.0f;
    static_cast<float*>(maskedHighImage->GetScalarPointer())[0] = 1000.0f;
    static_cast<float*>(maskedNanImage->GetScalarPointer())[0] =
        (std::numeric_limits<float>::quiet_NaN)();
    auto validityMask = GetMask(image);
    static_cast<unsigned char*>(validityMask->GetScalarPointer())[0] = 0;
    validityMask->Modified();

    GapSurfaceConfig maskedSurface = surfaceConfig;
    maskedSurface.isoMode = GapIsoMode::DataRangeRatio;
    maskedSurface.dataRangeRatio = 0.5;
    auto maskedLowOverlay = std::make_shared<OverlayStub>();
    auto maskedHighOverlay = std::make_shared<OverlayStub>();
    auto maskedNanOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService maskedLowService;
    GapAnalysisService maskedHighService;
    GapAnalysisService maskedNanService;
    GapViewRequest maskedLowRequest;
    maskedLowRequest.inputImage = maskedLowImage;
    maskedLowRequest.validityMask = validityMask;
    maskedLowRequest.surface = maskedSurface;
    maskedLowRequest.voidParams = voidParams;
    maskedLowRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        maskedLowOverlay);
    GapViewRequest maskedHighRequest;
    maskedHighRequest.inputImage = maskedHighImage;
    maskedHighRequest.validityMask = validityMask;
    maskedHighRequest.surface = maskedSurface;
    maskedHighRequest.voidParams = voidParams;
    maskedHighRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        maskedHighOverlay);
    GapViewRequest maskedNanRequest;
    maskedNanRequest.inputImage = maskedNanImage;
    maskedNanRequest.validityMask = validityMask;
    maskedNanRequest.surface = maskedSurface;
    maskedNanRequest.voidParams = voidParams;
    maskedNanRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        maskedNanOverlay);
    expect(StartDisplay(
            maskedLowService,
            std::move(maskedLowRequest),
            maskedLowImage)
            && StartDisplay(
                maskedHighService,
                std::move(maskedHighRequest),
                maskedHighImage)
            && StartDisplay(
                maskedNanService,
                std::move(maskedNanRequest),
                maskedNanImage),
        "Gap view should accept an aligned partial validity mask.");
    auto maskedLowLabels = maskedLowService.BuildLabelImage();
    auto maskedHighLabels = maskedHighService.BuildLabelImage();
    auto maskedNanLabels = maskedNanService.BuildLabelImage();
    const auto labelCount = maskedLowLabels
        ? maskedLowLabels->GetNumberOfPoints() : 0;
    const auto* maskedLowLabelValues = maskedLowLabels
        ? static_cast<const int*>(
            maskedLowLabels->GetScalarPointer())
        : nullptr;
    const auto* maskedHighLabelValues = maskedHighLabels
        ? static_cast<const int*>(
            maskedHighLabels->GetScalarPointer())
        : nullptr;
    const auto* maskedNanLabelValues = maskedNanLabels
        ? static_cast<const int*>(
            maskedNanLabels->GetScalarPointer())
        : nullptr;
    expect(maskedLowLabels
            && maskedHighLabels
            && maskedNanLabels
            && maskedLowLabelValues
            && maskedHighLabelValues
            && maskedNanLabelValues
            && labelCount == maskedHighLabels->GetNumberOfPoints()
            && labelCount == maskedNanLabels->GetNumberOfPoints()
            && std::equal(
                maskedLowLabelValues,
                maskedLowLabelValues + labelCount,
                maskedHighLabelValues)
            && std::equal(
                maskedLowLabelValues,
                maskedLowLabelValues + labelCount,
                maskedNanLabelValues)
            && maskedLowService.GetStatistics().voidVoxelCount
                == maskedHighService.GetStatistics().voidVoxelCount
            && maskedLowService.GetStatistics().voidVoxelCount
                == maskedNanService.GetStatistics().voidVoxelCount,
        "Mask-out source values must not affect the DefX result or DataRangeRatio domain.");
    expect(static_cast<const float*>(
                maskedLowImage->GetScalarPointer())[0] == -1000.0f
            && static_cast<const float*>(
                maskedHighImage->GetScalarPointer())[0] == 1000.0f
            && std::isnan(static_cast<const float*>(
                maskedNanImage->GetScalarPointer())[0]),
        "Temporary mask materialization must not modify caller-owned voxels.");

    auto maskedShortImage = vtkSmartPointer<vtkImageData>::New();
    maskedShortImage->CopyStructure(image);
    maskedShortImage->AllocateScalars(VTK_SHORT, 1);
    auto* shortVoxels = static_cast<short*>(
        maskedShortImage->GetScalarPointer());
    std::fill_n(shortVoxels, 125, static_cast<short>(100));
    shortVoxels[0] = 1000;
    shortVoxels[2 + 5 * (2 + 5 * 2)] = 0;
    auto maskedShortOverlay = std::make_shared<OverlayStub>();
    GapAnalysisService maskedShortService;
    GapViewRequest maskedShortRequest;
    maskedShortRequest.inputImage = maskedShortImage;
    maskedShortRequest.validityMask = validityMask;
    maskedShortRequest.surface = maskedSurface;
    maskedShortRequest.voidParams = voidParams;
    maskedShortRequest.sliceTargets.emplace_back(
        Orientation::Top_down,
        maskedShortOverlay);
    expect(StartDisplay(
            maskedShortService,
            std::move(maskedShortRequest),
            maskedShortImage),
        "A masked non-float input should be converted before DefX.");
    auto maskedShortLabels = maskedShortService.BuildLabelImage();
    const auto* maskedShortLabelValues = maskedShortLabels
        ? static_cast<const int*>(
            maskedShortLabels->GetScalarPointer())
        : nullptr;
    expect(maskedShortLabels
            && maskedHighLabels
            && maskedShortLabelValues
            && maskedHighLabelValues
            && maskedShortLabels->GetNumberOfPoints() == labelCount
            && std::equal(
                maskedShortLabelValues,
                maskedShortLabelValues + labelCount,
                maskedHighLabelValues)
            && maskedShortImage->GetScalarType() == VTK_SHORT
            && static_cast<const short*>(
                maskedShortImage->GetScalarPointer())[0] == 1000,
        "Masked conversion must preserve the caller image and the float-path result.");

    // 非 float 的正式图输入在转换后仍须产出可发布的全部 payload。
    auto graphShortInput = BuildGraphInput(maskedShortImage, validityMask);
    GapAnalysisService graphShortService;
    GapViewRequest graphShortRequest;
    graphShortRequest.graphInput = graphShortInput;
    graphShortRequest.surface = maskedSurface;
    graphShortRequest.voidParams = voidParams;
    graphShortRequest.sliceTargets.emplace_back(
        Orientation::Top_down, std::make_shared<OverlayStub>());
    int graphShortCompleteCount = 0;
    bool isGraphShortSucceeded = false;
    expect(graphShortService.StartView(std::move(graphShortRequest),
        [&](const bool isSucceeded) {
            ++graphShortCompleteCount;
            isGraphShortSucceeded = isSucceeded;
        }), "A non-float graph input must be admitted.");
    const auto graphShortDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (graphShortService.GetAnalysisState() == GapAnalysisState::Running
        && std::chrono::steady_clock::now() < graphShortDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    graphShortService.OnDisplayTick(maskedShortImage);
    GapAnalysisResult graphShortResult;
    const bool hasGraphShortResult = graphShortService.GetCompletedResult(graphShortResult);
    const auto& graphShortPayloads = graphShortResult.payloads;
    expect(hasGraphShortResult && graphShortPayloads
        && graphShortPayloads->labels && graphShortPayloads->labels->GetValid()
        && graphShortPayloads->mesh && graphShortPayloads->mesh->GetValid()
        && graphShortPayloads->voids && graphShortPayloads->voids->GetValid()
        && graphShortPayloads->statistics && graphShortPayloads->statistics->GetValid(),
        "Non-float graph conversion must preserve publication payloads.");
    if(hasGraphShortResult&&graphShortPayloads&&graphShortPayloads->mesh&&graphShortResult.voidMesh) {
        auto* geometry=graphShortResult.voidMesh.GetPointer();
        const auto& formal=*graphShortPayloads->mesh;
        bool same=geometry->GetNumberOfPoints()==static_cast<vtkIdType>(formal.GetPointValues().size()/3)
            &&geometry->GetNumberOfCells()==static_cast<vtkIdType>(formal.GetTriangleIndices().size()/3);
        for(vtkIdType i=0;same&&i<geometry->GetNumberOfPoints();++i){double point[3];geometry->GetPoint(i,point);
            for(unsigned c=0;c<3;++c)same=same&&point[c]==formal.GetPointValues()[static_cast<std::size_t>(3*i+c)];}
        auto* cells=geometry->GetPolys()->GetConnectivityArray();
        for(vtkIdType i=0;same&&i<cells->GetNumberOfValues();++i)same=static_cast<std::uint64_t>(cells->GetComponent(i,0))==formal.GetTriangleIndices()[static_cast<std::size_t>(i)];
        expect(same,"Worker VTK geometry must preserve the complete readonly publication values and connectivity.");
    }
    expect(CommitDisplayResult(graphShortService)
        && graphShortCompleteCount == 1 && isGraphShortSucceeded,
        "Non-float graph publication must complete exactly once.");
    if(hasGraphShortResult&&graphShortPayloads&&graphShortPayloads->mesh) {
        expect(graphShortResult.display&&graphShortResult.display->spatial
            && &graphShortResult.display->spatial->GetMesh()==graphShortPayloads->mesh.get(),
            "Graph display must read the exact frozen formal mesh.");
        RoiPrimitive half;half.shape=RoiShape::HalfSpace;half.origin={2.25,0,0};half.normal={1,0,0};
        const auto roi=RoiEvaluator::GetPreview(graphShortInput->graph,{graphShortInput->data->self,{{RoiNodeKind::Primitive,half}}});
        expect(roi.error==RoiError::None&&roi.roi,"Saved-parent display ROI must resolve against its source.");
        if(roi.roi&&graphShortResult.display&&graphShortResult.display->spatial) {
            GapAnalysisService childService;GapViewRequest childRequest;
            childRequest.graphInput=graphShortInput;childRequest.surface=surfaceConfig;childRequest.voidParams=voidParams;
            childRequest.savedParent=std::make_shared<const GapAnalysisResult>(graphShortResult);
            childRequest.isEvaluationOnly=true;childRequest.evaluationRoi=roi.roi;childRequest.roiPolicy=GapRoiPolicy::InsideOnly;
            childRequest.meshTargets={std::make_shared<OverlayStub>()};
            expect(childService.StartView(std::move(childRequest)),"Saved-parent child view must be accepted.");
            const auto childDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(childService.GetAnalysisState()==GapAnalysisState::Running&&std::chrono::steady_clock::now()<childDeadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            childService.OnDisplayTick(nullptr);GapAnalysisResult child;
            expect(childService.GetCompletedResult(child)&&child.payloads&&child.payloads->mesh&&child.display&&child.display->spatial
                && &child.display->spatial->GetMesh()==child.payloads->mesh.get()
                &&child.display!=graphShortResult.display&&child.display->spatial!=graphShortResult.display->spatial,
                "ROI child display must use child formal geometry rather than the parent's cached display.");
            expect(CommitDisplayResult(childService),"Saved-parent child display must commit independently.");
            childService.ExitView();childService.OnDisplayTick(nullptr);
        }
    }
    auto rawReplacement=vtkSmartPointer<vtkImageData>::New();rawReplacement->SetDimensions(5,5,5);rawReplacement->AllocateScalars(VTK_FLOAT,1);
    std::fill_n(static_cast<float*>(rawReplacement->GetScalarPointer()),125,100.F);*static_cast<float*>(rawReplacement->GetScalarPointer(1,1,1))=0.F;
    auto rawOverlay=std::make_shared<OverlayStub>();GapViewRequest rawRequest;rawRequest.inputImage=rawReplacement;rawRequest.surface=surfaceConfig;rawRequest.voidParams=voidParams;rawRequest.meshTargets={rawOverlay};
    expect(StartDisplay(graphShortService,std::move(rawRequest),rawReplacement),"A raw view may replace an already consumed graph view.");
    auto* rawMesh=vtkPolyData::SafeDownCast(rawOverlay->GetInput(0));
    expect(rawMesh&&rawMesh->GetBounds()[1]<2,"Graph-to-raw replacement must not reuse old spatial batches.");
    expect(graphShortService.ExitView(), "Non-float graph view must exit cleanly.");
    graphShortService.OnDisplayTick(nullptr);

    expect(maskedLowService.ExitView()
            && maskedHighService.ExitView()
            && maskedNanService.ExitView()
            && maskedShortService.ExitView(),
        "Masked Gap views should exit cleanly.");
    maskedLowService.OnDisplayTick(nullptr);
    maskedHighService.OnDisplayTick(nullptr);
    maskedNanService.OnDisplayTick(nullptr);
    maskedShortService.OnDisplayTick(nullptr);

    auto emptyMask = GetMask(image, 0);
    GapAnalysisService emptyMaskService;
    GapViewRequest emptyMaskRequest;
    emptyMaskRequest.inputImage = image;
    emptyMaskRequest.validityMask = emptyMask;
    emptyMaskRequest.surface = surfaceConfig;
    emptyMaskRequest.voidParams = voidParams;
    emptyMaskRequest.sliceTargets = sliceTargets;
    expect(!emptyMaskService.StartView(
            std::move(emptyMaskRequest)),
        "Gap view should reject a validity mask with no valid voxels.");
    expect(emptyMaskService.GetAnalysisState()
            == GapAnalysisState::Idle
            && !emptyMaskService.GetViewOn()
            && emptyMaskService.GetVoidRegions().empty(),
        "An all-zero mask must fail before DefX or display state changes.");

    auto nonFiniteValidImage = vtkSmartPointer<vtkImageData>::New();
    nonFiniteValidImage->DeepCopy(image);
    static_cast<float*>(nonFiniteValidImage->GetScalarPointer())[
        2 + 5 * (2 + 5 * 2)] =
            (std::numeric_limits<float>::quiet_NaN)();
    GapAnalysisService nonFiniteValidService;
    GapViewRequest nonFiniteValidRequest;
    nonFiniteValidRequest.inputImage = nonFiniteValidImage;
    nonFiniteValidRequest.validityMask = validityMask;
    nonFiniteValidRequest.surface = surfaceConfig;
    nonFiniteValidRequest.voidParams = voidParams;
    nonFiniteValidRequest.sliceTargets = sliceTargets;
    expect(!nonFiniteValidService.StartView(
            std::move(nonFiniteValidRequest)),
        "Gap view should reject a non-finite value inside the valid domain.");
    auto teardownService = std::make_shared<GapAnalysisService>();
    GapViewRequest teardownRequest;
    teardownRequest.inputImage = image;
    teardownRequest.surface = surfaceConfig;
    teardownRequest.voidParams = voidParams;
    teardownRequest.sliceTargets = sliceTargets;
    expect(teardownService->StartView(std::move(teardownRequest)),
        "Teardown view should bind its owner thread.");
    teardownService->ClearView();
    std::thread releaseThread([serviceOwner = std::move(teardownService)]() mutable {
        serviceOwner.reset();
    });
    releaseThread.join();

    GapAnalysisService callbackService;
    expect(callbackService.SetGapInput(image),
        "Callback cleanup service should accept isolated input.");
    GapSurfaceParams surfaceParams;
    surfaceParams.isoValue = 50.0f;
    surfaceParams.background = 0.0f;
    surfaceParams.material = 100.0f;
    callbackService.SetSurface(surfaceParams);
    callbackService.SetVoid(voidParams);
    bool hasLowCallback = false;
    expect(callbackService.StartAsync(
        [&](bool) { hasLowCallback = true; }),
        "Low-level callback task should be accepted.");
    const auto callbackDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (callbackService.GetAnalysisState()
            == GapAnalysisState::Running
        && std::chrono::steady_clock::now()
            < callbackDeadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }
    GapViewRequest blockedRequest;
    blockedRequest.inputImage = image;
    blockedRequest.surface = surfaceConfig;
    blockedRequest.voidParams = voidParams;
    blockedRequest.sliceTargets = sliceTargets;
    expect(!callbackService.StartView(std::move(blockedRequest)),
        "Pending service callback should reject a new view before state changes.");
    callbackService.ClearView();
    expect(!callbackService.GetDoneEvent(),
        "ClearView should clear the service-level callback doorbell.");
    callbackService.SendCallback();
    expect(!hasLowCallback,
        "ClearView should make the pending service callback unreachable.");

    GapViewRequest retryRequest;
    retryRequest.inputImage = image;
    retryRequest.surface = surfaceConfig;
    retryRequest.voidParams = voidParams;
    retryRequest.sliceTargets = sliceTargets;
    expect(callbackService.StartView(std::move(retryRequest)),
        "ClearView should release worker and callback slots for a new view.");
    callbackService.ClearView();

    // 掩膜结果已采用边界不确定契约；原有“等同最小值填充”的循环不再是正确预期。
    // 以独立整数几何覆盖有效域、边界、统计和显示，All 与路径回归共同执行。
    failureCount += GetGapMaskGeometryFailCount();
    return failureCount;
}

// 真值来自整数立方体与显式有效域；不调用另一条检测路径生成预期。
int GetGapMaskGeometryFailCount()
{
    int failures = 0;
    const auto expect = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    constexpr int side = 11, voxelCount = side * side * side;
    const auto index = [](int x, int y, int z) { return x + side * (y + side * z); };
    struct Scenario final {
        std::size_t formal = 0;
        std::size_t boundary = 0;
        bool useRatio = false;
        bool filter = false;
    };
    constexpr std::array<Scenario, 14> scenarios{{
        {27, 0}, {27, 0}, {0, 36}, {0, 0}, {0, 27}, {0, 18}, {0, 0},
        {0, 0}, {27, 0, true}, {27, 0}, {27, 0}, {0, 27}, {0, 0, false, true}, {0, 27}
    }};
    for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario) {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(side, side, side);
        image->SetSpacing(1, 1, 1); // 合成解析几何，体素体积为 1 mm^3。
        image->AllocateScalars(VTK_FLOAT, 1);
        auto* values = static_cast<float*>(image->GetScalarPointer());
        std::fill_n(values, voxelCount, 100.0f);
        auto mask = GetMask(image);
        auto* validity = static_cast<unsigned char*>(mask->GetScalarPointer());
        std::vector<unsigned char> expectedDisplay(voxelCount, 0);
        const auto hole = [&](int firstX, int lastX, int firstY = 4, int lastY = 6,
                              int firstZ = 4, int lastZ = 6) {
            for (int z = firstZ; z <= lastZ; ++z) for (int y = firstY; y <= lastY; ++y)
                for (int x = firstX; x <= lastX; ++x) {
                    expectedDisplay[index(x,y,z)] = 1;
                    values[index(x,y,z)] = 0;
                }
        };
        // 几何预期按显式体素集合推导：基础块为 3x3x3；相邻无效面只保留未遮挡的有效体素。
        if (scenario == 0 || scenario == 1 || scenario == 4 || scenario == 5
            || scenario == 8 || scenario == 9 || scenario == 10 || scenario == 11 || scenario == 12 || scenario == 13)
            hole(2, 4);
        // 场景1中孔与无效块之间仍有x=5、6两层材料，不发生面接触。
        if (scenario == 1 || scenario == 3 || scenario == 7 || scenario == 8 || scenario == 12) {
            for (int z = 4; z <= 6; ++z) for (int y = 4; y <= 6; ++y)
                for (int x = 7; x <= 8; ++x) {
                    validity[index(x,y,z)] = 0;
                    if (scenario == 7) values[index(x,y,z)] = 1000000.0f;
                    if (scenario == 8) values[index(x,y,z)] = -1000000.0f;
                }
        }
        if (scenario == 2) {
            hole(2, 3); hole(7, 8); // 两个 2x3x3 孔共 36 体素；三体素无效隔带令两块均属边界不确定。
            for (int z = 4; z <= 6; ++z) for (int y = 4; y <= 6; ++y)
                for (int x = 4; x <= 6; ++x) validity[index(x,y,z)] = 0;
        }
        // 场景3的有效域只有材料，无效块附近没有空气。
        if (scenario == 4) {
            // 27 孔经空气通道连到体积外部；将通道内侧 x=1 置无效后，孔整体边界不确定。
            for (int x = 0; x <= 1; ++x) values[index(x,5,5)] = 0;
            validity[index(1,5,5)] = 0;
        }
        if (scenario == 5) {
            // 无效面覆盖 x=4 的九个孔体素，剩余 18 个显示体素都邻接不确定域。
            for (int z = 4; z <= 6; ++z) for (int y = 4; y <= 6; ++y)
                validity[index(4,y,z)] = 0;
        }
        if (scenario == 6 || scenario == 7) {
            // 全部有效域为背景空气；远处无效体素不应生成正式孔或边界对象。
            std::fill_n(values, voxelCount, 0.0f);
            for (int z = 4; z <= 6; ++z) for (int y = 4; y <= 6; ++y)
                for (int x = 7; x <= 8; ++x) {
                    validity[index(x,y,z)] = 0;
                    if (scenario == 7) values[index(x,y,z)] = 1000000.0f;
                }
        }
        // 场景8有效域范围为0..100；无效域的-1,000,000不得改变相对比例0.5对应的阈值。
        if (scenario == 9) {
            // 无效 (5,7,7) 与孔只角接触；六邻接下孔仍是正式内部对象。
            validity[index(5,7,7)] = 0;
        }
        if (scenario == 10) {
            // 无效 (5,7,5) 与孔只棱接触；六邻接下孔仍是正式内部对象。
            validity[index(5,7,5)] = 0;
        }
        if (scenario == 11 || scenario == 13) {
            // 无效 (5,5,5) 与孔面接触；整个 3x3x3 连通孔成为边界不确定。
            validity[index(5,5,5)] = 0;
        }
        if (scenario == 12) {
            // 原始 27 体素孔低于 30 mm^3 筛选值；显示及正式/边界统计都应被筛除。
            std::fill(expectedDisplay.begin(), expectedDisplay.end(), 0);
        }
        if (scenario == 5) {
            std::fill(expectedDisplay.begin(), expectedDisplay.end(), 0);
            for (int z = 4; z <= 6; ++z) for (int y = 4; y <= 6; ++y)
                for (int x = 2; x <= 3; ++x) expectedDisplay[index(x,y,z)] = 1;
        }
        const std::vector<float> original(values, values + voxelCount);
        const std::vector<unsigned char> originalMask(validity, validity + voxelCount);
        image->Modified(); mask->Modified();
        GapAnalysisService service;
        GapViewRequest request;
        request.graphInput = BuildGraphInput(image, scenario == 0 ? nullptr : mask);
        if(scenario==13) {
            // 全域 ROI 包含全部27个边界孔体素；评价不能把边界片段升级成完整孔几何。
            RoiPrimitive all;all.shape=RoiShape::HalfSpace;all.origin={-1,0,0};all.normal={1,0,0};
            const auto roi=RoiEvaluator::GetPreview(request.graphInput->graph,
                {request.graphInput->data->self,{{RoiNodeKind::Primitive,all}}});
            expect(roi.error==RoiError::None && roi.roi,"whole-domain ROI resolves for uncertain pore");
            request.evaluationRoi=roi.roi;request.roiPolicy=GapRoiPolicy::InsideOnly;
        }
        request.surface.isoMode = scenarios[scenario].useRatio ? GapIsoMode::DataRangeRatio : GapIsoMode::AbsoluteValue;
        request.surface.dataRangeRatio = 0.5;
        request.surface.absoluteIsoValue = 50;
        request.surface.backgroundMean = 0;
        request.surface.materialMean = 100;
        request.voidParams.isFilterEnabled = scenarios[scenario].filter;
        request.voidParams.minVolumeMM3 = scenarios[scenario].filter ? 30 : 0;
        request.meshTargets = {std::make_shared<OverlayStub>()};
        std::shared_ptr<const GapDisplayData> completedDisplay;
        const bool started = StartDisplay(service, std::move(request), image, &completedDisplay);
        expect(started, "independent mask geometry must complete through real DefX");
        if (started) {
            const auto labels = service.BuildLabelImage();
            const auto* actual = labels ? static_cast<const int*>(labels->GetScalarPointer()) : nullptr;
            expect(actual && labels->GetNumberOfPoints() == voxelCount, "mask geometry labels have expected shape");
            if (actual && labels->GetNumberOfPoints() == voxelCount) {
                std::size_t invalid = 0, missed = 0, extra = 0;
                for (int i = 0; i < voxelCount; ++i) {
                    invalid += scenario != 0 && !validity[i] && actual[i] != 0;
                    missed += expectedDisplay[i] && actual[i] == 0;
                    extra += !expectedDisplay[i] && actual[i] != 0;
                }
                const auto expectedVoxels = std::count(expectedDisplay.begin(), expectedDisplay.end(), 1);
                const auto stats = service.GetStatistics();
                const auto regions = service.GetVoidRegions();
                std::size_t formalRegionVoxels = 0, boundaryRegionVoxels = 0;
                bool regionStatesValid = true, boundaryGeometryValid = true;
                for (const auto& region : regions) {
                    if (region.boundaryStatus == GapBoundaryStatus::BoundaryUncertain) {
                        boundaryRegionVoxels += static_cast<std::size_t>(region.voxelCount);
                        boundaryGeometryValid &= !region.hasWholeObjectGeometry;
                    } else if (region.boundaryStatus == GapBoundaryStatus::Interior) {
                        formalRegionVoxels += static_cast<std::size_t>(region.voxelCount);
                    } else regionStatesValid = false;
                }
                const Scenario expected = scenarios[scenario];
                std::cout << "GAP_MASK_GEOMETRY scenario=" << scenario
                          << " expected_display=" << expectedVoxels << " formal=" << stats.voidVoxelCount
                          << " boundary=" << stats.boundaryUncertainVoxelCount << " invalid=" << invalid
                          << " missed=" << missed << " extra=" << extra << " regions=" << regions.size() << '\n';
                expect(invalid == 0, "invalid domain must contain no pore labels");
                expect(missed == 0 && extra == 0, "labels must match the independently defined display geometry");
                expect(stats.voidCount == (expected.formal ? 1u : 0u)
                           && stats.voidVoxelCount == expected.formal
                           && std::abs(stats.voidVolumeMM3 - expected.formal) <= 1e-6,
                       "formal count, voxels and volume match independent geometry");
                expect(stats.boundaryUncertainCount == (expected.boundary ? (scenario == 2 ? 2u : 1u) : 0u)
                           && stats.boundaryUncertainVoxelCount == expected.boundary
                           && std::abs(stats.boundaryUncertainVolumeMM3 - expected.boundary) <= 1e-6,
                       "boundary count, voxels and volume match independent geometry");
                expect(formalRegionVoxels == expected.formal && boundaryRegionVoxels == expected.boundary
                           && regionStatesValid && boundaryGeometryValid,
                       "visible objects retain boundary state and omit whole geometry for uncertain regions");
                expect(regions.size() == (expected.formal ? 1u : 0u) + (expected.boundary ? (scenario == 2 ? 2u : 1u) : 0u),
                       "all independently expected visible pore objects are retained");
                if (scenario == 2) expect(actual[index(2,5,5)] != 0 && actual[index(7,5,5)] != 0
                    && actual[index(2,5,5)] != actual[index(7,5,5)], "two valid pore components must remain distinct");
                expect(completedDisplay && completedDisplay->labels,
                       "completed display candidate retains its label lookup table");
                if (completedDisplay && completedDisplay->labels) {
                    GapDisplayParams recolor;
                    recolor.mode = GapColorMode::Constant;
                    recolor.constantColor = {0.25, 0.75, 0.125};
                    expect(service.SetDisplay(recolor), "color mode change accepts boundary-containing result");
                    const auto recolored = GapDisplayData::CreateColors(*completedDisplay, recolor);
                    for (const auto& region : regions) {
                        const auto checkAmber = [&](const std::shared_ptr<const GapDisplayData>& display) {
                            const auto* rgba = display && display->labels
                                ? display->labels->MapValue(region.id) : nullptr;
                            return rgba && rgba[0] == 255 && rgba[1] == 166 && rgba[2] == 0 && rgba[3] == 255;
                        };
                        if (region.boundaryStatus == GapBoundaryStatus::BoundaryUncertain) {
                            expect(checkAmber(completedDisplay) && checkAmber(recolored),
                                   "boundary labels retain fixed amber RGBA after display color changes");
                        } else {
                            const auto* rgba = recolored && recolored->labels
                                ? recolored->labels->MapValue(region.id) : nullptr;
                            expect(rgba && rgba[0] == 64 && rgba[1] == 191 && rgba[2] == 32 && rgba[3] == 255,
                                   "formal regions continue using the selected color ramp");
                        }
                    }
                }
            }
        }
        expect(std::equal(original.begin(), original.end(), values)
                   && std::equal(originalMask.begin(), originalMask.end(), validity),
               "mask geometry analysis leaves source scalar and validity data unchanged");
        service.ClearView();
    }
    return failures;
}

namespace {
class GapRealViews final : public FeatureViewDirectory {
public:
    std::shared_ptr<OverlayStub> overlay = std::make_shared<OverlayStub>();
    std::vector<HostFeatureView> GetViews(const HostViewTargets&) const override
    { return {{"gap-real", HostRenderViewRole::Primary3D}}; }
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string&) const override { return {}; }
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string&) const override { return overlay; }
    std::optional<HostInputView> GetInputView(const HostViewTarget&) const override { return {}; }
};
class GapRealControl final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { return true; }
    bool SetActiveViews(const std::vector<std::string>&) override { return true; }
    bool SetViewStatus(const std::vector<std::string>&, const std::string&) override { return true; }
    bool SendSceneDelta(FeatureSceneDelta) override { return true; }
    bool SendOwnerComplete(std::function<void()> complete) override {
        pending.push_back(std::move(complete));
        return true;
    }
    void Drain() {
        auto ready = std::move(pending);
        pending.clear();
        for (auto& complete : ready) complete();
    }
private:
    std::vector<std::function<void()>> pending;
};
}

// 输入规格来自 integration-20261011/host/consumer-real-input.json；只验证真实小样本公共请求链。
int GetGapRealSmallFailCount(const char* rawPath, const char* referencePath, bool writeReference)
{
    int failures = 0;
    const auto expect = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
        return value;
    };
    constexpr int side = 32, count = side * side * side;
    std::vector<float> original(count);
    std::ifstream input(std::filesystem::u8path(rawPath), std::ios::binary);
    if (!expect(bool(input.read(reinterpret_cast<char*>(original.data()), count * sizeof(float)))
                    && input.peek() == std::char_traits<char>::eof(), "real ROI must be exactly 32^3 float32 values")) return failures;
    for (int masked = 0; masked < 2; ++masked) {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(side, side, side);
        image->SetSpacing(.1537, .1537, .1537);
        image->SetOrigin(74.3908, 74.3908, 74.3908);
        image->AllocateScalars(VTK_FLOAT, 1);
        auto* scalars = static_cast<float*>(image->GetScalarPointer());
        std::copy(original.begin(), original.end(), scalars);
        auto mask = GetMask(image);
        auto* validity = static_cast<unsigned char*>(mask->GetScalarPointer());
        if (masked) for (int z=0;z<side;++z) for (int y=0;y<side;++y)
            for (int x=24;x<side;++x) validity[x+side*(y+side*z)]=0;
        image->Modified(); mask->Modified();
        auto data = std::make_shared<TestDataPort>();
        const auto source = data->SetPrimaryImage(image, masked ? mask.GetPointer() : nullptr);
        if (!expect(bool(source), "real ROI graph publication succeeds")) continue;
        GapHostConfig config;
        config.inputViews.viewIds = {"gap-real"};
        config.defaultStart.targetViews = config.inputViews;
        config.defaultStart.surface.isoMode = GapIsoMode::AbsoluteValue;
        config.defaultStart.surface.absoluteIsoValue = .172;
        config.defaultStart.surface.backgroundMean = -1.617f;
        config.defaultStart.surface.materialMean = .453f;
        config.keys.switchOverlay.keyCode = 'j';
        config.keys.exit.keySym = "Escape";
        auto feature = std::make_shared<GapHostFeature>(config);
        HostFeatureContext context;
        context.data = data;
        context.views = std::make_shared<GapRealViews>();
        auto control = std::make_shared<GapRealControl>();
        context.host = control;
        if (!expect(feature->AttachHost(context), "real ROI feature attaches through existing SPI")) continue;
        GapHostRequest request; request.action=GapHostAction::Start; request.start=config.defaultStart;
        GapHostResult completed;
        int callbacks = 0;
        const bool accepted = feature->SendRequest(std::move(request), [&](GapHostResult result) {
            completed=std::move(result); ++callbacks;
        }).status == GapAdmissionStatus::Accepted;
        expect(accepted, "real ROI public Gap Start is accepted");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while (accepted && callbacks==0 && std::chrono::steady_clock::now()<deadline) {
            feature->OnHostTick();
            control->Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 这是测试进程保护，避免超时后异步回调访问已离开的测试栈。
        if (accepted && callbacks==0) { expect(false,"real ROI completion timed out"); std::abort(); }
        const bool succeeded=callbacks==1
            && (completed.status==GapResultStatus::Succeeded || completed.status==GapResultStatus::SucceededWithDisplayFailure);
        expect(succeeded, "real ROI public Gap Start completes exactly once");
        if (succeeded) {
            const auto graph=data->GetDataGraph();
            const auto node=data->GetData(graph,completed.labelMap);
            const auto labels=node?std::dynamic_pointer_cast<const LabelMap3DPayload>(node->payload):nullptr;
            const auto reader=feature->GetResultReader(completed.resultSet);
            expect(completed.sourceRevision==source->data->self && labels && labels->GetValueCount()==count
                && reader && reader->GetMetadata().source==source->data->self,
                "real ROI result identity, label shape and reader source agree");
            if (labels) {
                std::size_t labeled=0,outside=0;
                std::visit([&](const auto& values) {
                    if (!values) return;
                    for(std::size_t i=0;i<values->size();++i) {
                        labeled+=(*values)[i]!=0;
                        outside+=masked && !validity[i] && (*values)[i]!=0;
                    }
                },labels->GetValues());
                std::uint64_t formalObjects=0,boundaryObjects=0,formalObjectVoxels=0,boundaryObjectVoxels=0;
                bool objectsReadable=bool(reader);
                if(reader) {
                    std::uint64_t offset=0;
                    do {
                        const auto page=reader->GetObjects(offset,4096);
                        if(page.error!=GapReadError::None || page.next<=offset && page.total>offset) {
                            objectsReadable=false;break;
                        }
                        for(const auto& object:page.objects) {
                            if(object.boundaryStatus==GapBoundaryStatus::BoundaryUncertain) {
                                ++boundaryObjects;boundaryObjectVoxels+=object.voxelCount;
                            } else {
                                ++formalObjects;formalObjectVoxels+=object.voxelCount;
                            }
                        }
                        offset=page.next;
                        if(offset>=page.total)break;
                    } while(objectsReadable);
                }
                std::cout << "GAP_REAL_SMALL masked=" << masked << " labeled=" << labeled
                    << " formal_voxels=" << completed.statistics.voidVoxelCount
                    << " boundary_voxels=" << completed.statistics.boundaryUncertainVoxelCount
                    << " formal_objects=" << formalObjects << " boundary_objects=" << boundaryObjects
                    << " outside=" << outside << '\n';

                if (!masked && referencePath && *referencePath) {
                    std::vector<std::int32_t> current;
                    std::visit([&](const auto& values) {
                        if(values)for(const auto value:*values)current.push_back(static_cast<std::int32_t>(value));
                    },labels->GetValues());
                    const auto bytes=static_cast<std::streamsize>(current.size()*sizeof(std::int32_t));
                    if (writeReference) {
                        std::ofstream saved(std::filesystem::u8path(referencePath),std::ios::binary);
                        expect(bool(saved.write(reinterpret_cast<const char*>(current.data()),bytes)),"save pre-fix real unmasked labels");
                    } else {
                        std::vector<std::int32_t> expected(current.size());
                        std::ifstream saved(std::filesystem::u8path(referencePath),std::ios::binary);
                        expect(bool(saved.read(reinterpret_cast<char*>(expected.data()),bytes))
                            && saved.peek()==std::char_traits<char>::eof() && expected==current,
                            "real unmasked labels equal the pre-fix reference at every voxel");
                    }
                }
                expect(labeled==completed.statistics.voidVoxelCount+completed.statistics.boundaryUncertainVoxelCount,
                    "real ROI label count agrees with formal and boundary statistics");
                expect(objectsReadable && formalObjects==completed.statistics.voidCount
                    && boundaryObjects==completed.statistics.boundaryUncertainCount
                    && formalObjectVoxels==completed.statistics.voidVoxelCount
                    && boundaryObjectVoxels==completed.statistics.boundaryUncertainVoxelCount,
                    "real ROI object pages preserve formal and boundary object counts and voxels");
                expect(outside==0,"real ROI masked domain contains no formal pore labels");
                const auto& grid=labels->GetGeometry();
                expect(grid.spacing==std::array<double,3>{.1537,.1537,.1537}
                    && grid.origin==std::array<double,3>{74.3908,74.3908,74.3908},
                    "real ROI labels preserve measured spacing and source origin");
            }
        }
        expect(std::equal(original.begin(),original.end(),scalars),"real ROI scalar input remains unchanged");
        if (succeeded) {
            GapHostRequest exit; exit.action = GapHostAction::Exit;
            int exitCallbacks = 0;
            const bool exitAccepted = feature->SendRequest(std::move(exit), [&](GapHostResult result) {
                ++exitCallbacks;
                expect(result.status == GapResultStatus::Succeeded, "real ROI Exit completes successfully");
            }).status == GapAdmissionStatus::Accepted;
            expect(exitAccepted, "real ROI public Exit is accepted");
            const auto exitDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while (exitAccepted && exitCallbacks==0 && std::chrono::steady_clock::now()<exitDeadline) {
                feature->OnHostTick(); control->Drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (exitAccepted && exitCallbacks==0) { expect(false,"real ROI Exit timed out"); std::abort(); }
            expect(exitCallbacks==1 && !feature->GetState().isViewActive, "real ROI Exit clears the view exactly once");
        }
        expect(feature->DetachHost(),"real ROI feature detaches after completion");
    }
    return failures;
}


// 运行时探针：两个 2^3 空气块分别只有面、棱、角接触；几何不依赖检测输出。
int GetGapConnectivityFailCount(const char* referencePath, bool writeReference)
{
    int failures = 0;
    std::vector<int> reference;
    for (int contact = 0; contact < 3; ++contact) {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(11,11,11); image->SetSpacing(1,1,1);
        image->AllocateScalars(VTK_FLOAT,1);
        auto* values=static_cast<float*>(image->GetScalarPointer());
        std::fill_n(values,1331,100.f);
        const auto block=[&](int bx,int by,int bz) {
            for(int z=bz;z<bz+2;++z)for(int y=by;y<by+2;++y)for(int x=bx;x<bx+2;++x)
                values[x+11*(y+11*z)]=0;
        };
        block(3,3,3); block(5,contact>=1?5:3,contact>=2?5:3);
        image->Modified();
        GapAnalysisService service; GapViewRequest request;
        request.graphInput=BuildGraphInput(image);
        request.surface.isoMode=GapIsoMode::AbsoluteValue;
        request.surface.absoluteIsoValue=50;request.surface.backgroundMean=0;request.surface.materialMean=100;
        request.meshTargets={std::make_shared<OverlayStub>()};
        if(!StartDisplay(service,std::move(request),image)){++failures;continue;}
        auto labels=service.BuildLabelImage();
        const auto* data=labels?static_cast<const int*>(labels->GetScalarPointer()):nullptr;
        const auto regions=service.GetVoidRegions(); const auto stats=service.GetStatistics();
        std::cout << "GAP_CONNECTIVITY contact=" << contact << " regions=" << regions.size()
            << " voxels=" << stats.voidVoxelCount << " volume_mm3=" << stats.voidVolumeMM3 << '\n';
        const std::size_t expectedRegions = contact == 0 ? 1 : 2;
        if(!data || stats.voidVoxelCount!=16 || regions.size()!=expectedRegions){++failures;}else reference.insert(reference.end(),data,data+1331);
        reference.push_back(static_cast<int>(regions.size()));
        reference.push_back(static_cast<int>(stats.voidVoxelCount));
        reference.push_back(static_cast<int>(stats.objectVoxelCount));
        for(const auto& region:regions) {
            reference.push_back(region.id); reference.push_back(static_cast<int>(region.voxelCount));
        }
        service.ClearView();
    }
    if(referencePath && *referencePath) {
        const auto bytes=static_cast<std::streamsize>(reference.size()*sizeof(int));
        if(writeReference) {
            std::ofstream output(std::filesystem::u8path(referencePath),std::ios::binary);
            if(!output.write(reinterpret_cast<const char*>(reference.data()),bytes))++failures;
        }else {
            std::ifstream input(std::filesystem::u8path(referencePath),std::ios::binary);
            std::vector<int> expected(reference.size());
            if(!input.read(reinterpret_cast<char*>(expected.data()),bytes)
                || input.peek()!=std::char_traits<char>::eof() || expected!=reference)++failures;
        }
    }
    return failures;
}
