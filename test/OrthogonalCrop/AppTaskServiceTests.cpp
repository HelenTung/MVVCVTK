#include "Tasks/AppDataExportTaskService.h"
#include "Tasks/AppDataLoadTaskService.h"
#include "Algorithms/CropAlgorithm.h"
#include "AppState.h"
#include "AppStateEvents.h"
#include "Data/DataManager.h"
#include "Data/VolumeTypes.h"
#include "PlanarTestSuites.h"
#include "Render/CropShaderController.h"
#include "Render/Strategies/VolumeStrategy.h"
#include "Services/AppService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <vtkImageData.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkOBJReader.h>
#include <vtkPNGReader.h>
#include <vtkPLYReader.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSTLReader.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkWeakPointer.h>

namespace {
void SetExpect(bool isPassed, const char* message, int& failureCount)
{
    if (isPassed) return;
    ++failureCount;
    std::cerr << "[AppTaskTests] " << message << '\n';
}

class DataStub final : public AbstractDataManager {
protected:
    ImageSnapshot GetImageSnapshot() const override { return imageSnapshot; }

public:
    vtkSmartPointer<vtkImageData> GetVtkImage() const override { return nullptr; }
    ImageState GetImageState() const override { return {}; }
    std::array<double, 2> GetScalarRange() const override { return { 0.0, 0.0 }; }
    std::array<double, 3> GetSpacing() const override { return { 1.0, 1.0, 1.0 }; }
    bool SetSpacing(const std::array<double, 3>&) override { return true; }
    DataVersion GetDataVersion() const override { return 0; }

    bool SetDataLoaded(const std::string& path, const VolumeLayout& layout) override
    {
        loadedPath = path;
        loadedDims = layout.GetDimensions();
        if (isThrowNeeded) throw std::runtime_error("load failure");
        return isLoadSuccess;
    }

    bool SetFromBuffer(const VolumeBuffer& buffer) override
    {
        loadedVoxels = buffer.GetVoxels();
        loadedDims = buffer.GetLayout().GetDimensions();
        if (isThrowNeeded) throw std::runtime_error("reload failure");
        return isLoadSuccess;
    }

    bool SetCurrentFromPending(bool& hasPending) override
    {
        hasPending = false;
        return true;
    }
    bool ClearPending() override { return true; }
    bool ExportData(
        const ImageSnapshot& snapshot,
        const std::string& outputDir,
        const std::string& extension,
        double isoValue,
        const std::array<double, 16>&) override
    {
        exportedSnapshot = snapshot;
        exportedDir = outputDir;
        exportedExtension = extension;
        exportedIso = isoValue;
        return true;
    }
    bool ExportSlices(const std::string&, Orientation, const WindowLevelParams&,
        const std::array<double, 16>&) override { return false; }

    ImageSnapshot imageSnapshot;
    ImageSnapshot exportedSnapshot;
    std::string exportedDir;
    std::string exportedExtension;
    double exportedIso = 0.0;
    std::string loadedPath;
    std::array<int, 3> loadedDims{};
    std::vector<float> loadedVoxels;
    bool isLoadSuccess = true;
    bool isThrowNeeded = false;
};

class DataManagerProbe final : public BaseDataManager {
public:
    bool SetDataLoaded(
        const std::string&,
        const VolumeLayout&) override
    {
        return false;
    }

    bool SetInitial(
        vtkSmartPointer<vtkImageData> image)
    {
        return SetOwnedImage(std::move(image));
    }

    bool SetCandidate(
        ImageState state,
        const ImageSnapshot& expectedSnapshot,
        ImageSnapshot& publishedSnapshot)
    {
        return SetCurrentData(
            std::move(state),
            expectedSnapshot,
            publishedSnapshot);
    }

    ImageSnapshot GetSnapshot() const
    {
        return GetImageSnapshot();
    }
};

void StartVolumeTypes(int& failureCount)
{
    SetExpect(!VolumeLayout::Create({ 0, 2, 3 }, { 1, 1, 1 }, { 0, 0, 0 }),
        "zero dimension must fail", failureCount);
    SetExpect(!VolumeLayout::Create({ 2, 2, 3 }, { 1, 0, 1 }, { 0, 0, 0 }),
        "non-positive spacing must fail", failureCount);
    const auto layout = VolumeLayout::Create(
        { 2, 2, 3 }, { 0.5f, 1.0f, 2.0f }, { 3.0f, 4.0f, 5.0f });
    SetExpect(layout && layout->GetVoxelCount() == 12
        && layout->GetByteCount() == 12 * sizeof(float),
        "valid layout counts must be exact", failureCount);
    if (!layout) return;
    SetExpect(!VolumeBuffer::Create(std::vector<float>(11), *layout)
        && !VolumeBuffer::Create(std::vector<float>(13), *layout),
        "short and long owning buffers must fail", failureCount);
}

void StartOwningTasks(int& failureCount)
{
    auto dataManager = std::make_shared<DataStub>();
    AppDataLoadTaskService service(dataManager);
    auto layout = VolumeLayout::Create(
        { 2, 2, 2 }, { 1, 1, 1 }, { 0, 0, 0 });
    if (!layout) {
        ++failureCount;
        return;
    }

    std::vector<float> source{ 0, 1, 2, 3, 4, 5, 6, 7 };
    auto buffer = VolumeBuffer::Create(std::move(source), *layout);
    auto reloadTask = buffer
        ? service.BuildReloadTask(std::move(*buffer)) : std::nullopt;
    SetExpect(reloadTask.has_value(), "owning reload task must be built", failureCount);
    if (reloadTask) {
        auto result = reloadTask->get_future();
        (*reloadTask)();
        SetExpect(result.get() && dataManager->loadedVoxels
            == std::vector<float>({ 0, 1, 2, 3, 4, 5, 6, 7 }),
            "task must retain voxels after caller storage is destroyed", failureCount);
    }

    auto fileTask = service.BuildLoadFileTask("volume.raw", *layout);
    SetExpect(fileTask.has_value(), "file task must be built", failureCount);
    if (fileTask) {
        auto result = fileTask->get_future();
        (*fileTask)();
        SetExpect(result.get() && dataManager->loadedPath == "volume.raw"
            && dataManager->loadedDims == std::array<int, 3>{ 2, 2, 2 },
            "file task must retain path and layout", failureCount);
    }

    dataManager->isThrowNeeded = true;
    auto failedTask = service.BuildLoadFileTask("throw.raw", *layout);
    if (failedTask) {
        auto result = failedTask->get_future();
        (*failedTask)();
        SetExpect(!result.get(), "worker exceptions must become false", failureCount);
    }
}

vtkSmartPointer<vtkImageData> BuildExportImage()
{
    auto image =
        vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->SetSpacing(0.5, 1.0, 1.5);
    image->AllocateScalars(VTK_FLOAT, 1);
    for (int z = 0; z < 4; ++z) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                image->SetScalarComponentFromFloat(
                    x, y, z, 0,
                    static_cast<float>(x + y + z));
            }
        }
    }
    return image;
}

void StartExportSnapshot(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataStub>();
    auto firstState =
        std::make_shared<ImageState>();
    firstState->image = BuildExportImage();
    firstState->version = 1;
    dataManager->imageSnapshot = firstState;

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    state->SetIsoValue(2.5);
    AppDataExportTaskService service(
        dataManager, state);
    auto task = service.BuildDataTask(
        "exports", ".ply");

    auto secondState =
        std::make_shared<ImageState>();
    secondState->image = BuildExportImage();
    secondState->version = 2;
    dataManager->imageSnapshot = secondState;
    state->SetIsoValue(4.5);

    SetExpect(task.has_value(),
        "data export task should accept a valid snapshot",
        failureCount);
    if (!task) return;
    auto result = task->get_future();
    (*task)();
    SetExpect(result.get()
            && dataManager->exportedSnapshot
                == firstState
            && dataManager->exportedDir
                == "exports"
            && dataManager->exportedExtension
                == ".ply"
            && dataManager->exportedIso == 2.5,
        "data export must preserve target and admission-time snapshots",
        failureCount);
}

void StartExportFiles(int& failureCount)
{
    DataManagerProbe dataManager;
    SetExpect(
        dataManager.SetInitial(BuildExportImage()),
        "data export needs an image snapshot",
        failureCount);
    const auto snapshot =
        dataManager.GetSnapshot();
    const auto uniqueId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();
    const auto outputDir =
        std::filesystem::temp_directory_path()
        / std::filesystem::u8path(
            u8"MVVCVTK_网格")
        / std::to_string(uniqueId);
    std::error_code error;
    std::filesystem::create_directories(
        outputDir, error);
    const std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 10.0,
        0.0, 1.0, 0.0, 20.0,
        0.0, 0.0, 1.0, 30.0,
        0.0, 0.0, 0.0, 1.0
    };

    const auto plyPath =
        outputDir / "4x4x4_transform.ply";
    const auto stlPath =
        outputDir / "4x4x4_transform.stl";
    const auto objPath =
        outputDir / "4x4x4_transform.obj";
    SetExpect(!error
            && dataManager.ExportData(
                snapshot,
                outputDir.u8string(), ".ply",
                2.5, modelToWorld)
            && dataManager.ExportData(
                snapshot,
                outputDir.u8string(), ".stl",
                2.5, modelToWorld)
            && dataManager.ExportData(
                snapshot,
                outputDir.u8string(), ".obj",
                2.5, modelToWorld),
        "PLY, STL and OBJ should use one export entry",
        failureCount);

    auto plyReader =
        vtkSmartPointer<vtkPLYReader>::New();
    plyReader->SetFileName(
        plyPath.u8string().c_str());
    plyReader->Update();
    auto stlReader =
        vtkSmartPointer<vtkSTLReader>::New();
    stlReader->SetFileName(
        stlPath.u8string().c_str());
    stlReader->Update();
    auto objReader =
        vtkSmartPointer<vtkOBJReader>::New();
    objReader->SetFileName(
        objPath.u8string().c_str());
    objReader->Update();
    const auto getIsMeshValid =
        [](vtkPolyData* mesh) {
            if (!mesh
                || mesh->GetNumberOfPoints() == 0
                || mesh->GetNumberOfCells() == 0) {
                return false;
            }
            double bounds[6] = {};
            mesh->GetBounds(bounds);
            return bounds[0] >= 10.0
                && bounds[2] >= 20.0
                && bounds[4] >= 30.0;
        };
    SetExpect(
        getIsMeshValid(plyReader->GetOutput())
            && getIsMeshValid(
                stlReader->GetOutput())
            && getIsMeshValid(
                objReader->GetOutput()),
        "mesh files should contain baked world coordinates",
        failureCount);

    SetExpect(
        dataManager.ExportData(
            snapshot,
            outputDir.u8string(), ".raw",
            2.5, modelToWorld)
            && std::filesystem::exists(
                outputDir
                / "4x4x4_transform.raw"),
        "Raw should reuse the same export entry",
        failureCount);

    auto invalidMatrix = modelToWorld;
    invalidMatrix[0] =
        std::numeric_limits<double>::quiet_NaN();
    auto singularMatrix = modelToWorld;
    singularMatrix[0] = 0.0;
    SetExpect(
        !dataManager.ExportData(
            snapshot, outputDir.u8string(), ".raw",
            2.5, invalidMatrix)
            && !dataManager.ExportData(
                snapshot, outputDir.u8string(), ".raw",
                2.5, singularMatrix),
        "Raw should reject non-finite and singular transforms",
        failureCount);

    auto maskedState =
        std::make_shared<ImageState>(*snapshot);
    auto emptyMask =
        vtkSmartPointer<vtkImageData>::New();
    emptyMask->CopyStructure(snapshot->image);
    emptyMask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            emptyMask->GetScalarPointer()),
        emptyMask->GetNumberOfPoints(),
        static_cast<unsigned char>(0));
    maskedState->validityMask = emptyMask;
    SetExpect(
        !dataManager.ExportData(
            maskedState,
            outputDir.u8string(), ".ply",
            2.5, modelToWorld),
        "mesh export should consume the frozen validity mask",
        failureCount);

    std::filesystem::remove_all(
        outputDir, error);
}

void StartStateGate(int& failureCount)
{
    auto broadcaster = std::make_shared<SharedStateBroadcaster>();
    auto firstOwner = std::make_shared<int>(1);
    auto secondOwner = std::make_shared<int>(2);
    std::atomic<int> secondCount{ 0 };
    broadcaster->SetObserver(firstOwner, [](UpdateFlags) {
        throw std::runtime_error("observer failure");
    });
    broadcaster->SetObserver(secondOwner, [&](UpdateFlags) { ++secondCount; });
    auto state = std::make_shared<SharedInteractionState>(broadcaster);
    SetExpect(state->StartLoad(LoadEventKind::Reload),
        "reload admission must start", failureCount);
    bool nestedResult = true;
    auto nestedOwner = std::make_shared<int>(3);
    broadcaster->SetObserver(nestedOwner, [&](UpdateFlags) {
        nestedResult = state->SetReloadLoadFailed();
    });
    SetExpect(state->SetReloadDataReady(0.0, 1.0, { 1.0, 1.0, 1.0 }),
        "outer terminal must publish", failureCount);
    SetExpect(secondCount.load() == 1 && !nestedResult,
        "observer failure and terminal reentry must be isolated", failureCount);
    SetExpect(state->ResetLoad(LoadEventKind::Reload),
        "published terminal must release admission", failureCount);
}

void StartMaskSnapshot(int& failureCount)
{
    DataManagerProbe dataManager;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 1, 1);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* values = static_cast<float*>(
        image->GetScalarPointer());
    values[0] = 0.0f;
    values[1] = 100.0f;
    SetExpect(dataManager.SetInitial(image),
        "initial image snapshot should publish",
        failureCount);

    const auto expected = dataManager.GetSnapshot();
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* maskValues =
        static_cast<unsigned char*>(
            mask->GetScalarPointer());
    maskValues[0] = 255;
    maskValues[1] = 0;

    ImageState candidate = *expected;
    candidate.validityMask = mask;
    ImageSnapshot publishedSnapshot;
    SetExpect(dataManager.SetCandidate(
            candidate,
            expected,
            publishedSnapshot),
        "image and validity mask should publish as one CAS batch",
        failureCount);
    const auto current = dataManager.GetSnapshot();
    SetExpect(current && publishedSnapshot == current
            && current->version == expected->version + 1
            && current->validityMask.GetPointer()
                == mask.GetPointer(),
        "published mask should share the current ImageState version",
        failureCount);
    const auto currentVersion =
        current ? current->version : 0;
    SetExpect(!dataManager.SetCandidate(
            candidate,
            expected,
            publishedSnapshot)
            && !publishedSnapshot
            && dataManager.GetDataVersion()
                == currentVersion,
        "a stale expected snapshot must not replace current image or mask",
        failureCount);

    const auto uniqueId =
        std::chrono::steady_clock::now()
            .time_since_epoch().count();
    const auto outputDir =
        std::filesystem::temp_directory_path()
        / ("MVVCVTK_mask_"
            + std::to_string(uniqueId));
    const std::array<double, 16> identity = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    const bool isExported = dataManager.ExportSlices(
        outputDir.u8string(),
        Orientation::Top_down,
        { 100.0, 50.0 },
        identity);
    auto reader = vtkSmartPointer<vtkPNGReader>::New();
    reader->SetFileName(
        (outputDir / "Top_down_0000.png")
            .u8string().c_str());
    if (isExported) {
        reader->Update();
    }
    auto* output = reader->GetOutput();
    const auto* outputValues =
        output && output->GetNumberOfPoints() == 2
        ? static_cast<const unsigned char*>(
            output->GetScalarPointer())
        : nullptr;
    SetExpect(isExported
            && outputValues
            && outputValues[0] == 0
            && outputValues[1] == 0,
        "slice export should write mask=0 voxels as background",
        failureCount);
    std::error_code error;
    std::filesystem::remove_all(outputDir, error);
}

void StartInputSwap(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataManagerProbe>();
    auto firstImage =
        vtkSmartPointer<vtkImageData>::New();
    firstImage->SetDimensions(4, 4, 4);
    firstImage->AllocateScalars(VTK_FLOAT, 1);
    SetExpect(dataManager->SetInitial(firstImage),
        "render input swap needs an initial image",
        failureCount);

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    VizService service(
        dataManager, state, broadcaster);
    auto renderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    service.SetRenderContext(
        renderWindow, renderer);
    service.SetVizMode(VizMode::Volume);
    SetExpect(service.SendReloadUpdate(),
        "initial render pipeline should build",
        failureCount);
    auto* firstProp = service.GetMainProp();
    bool isWrongThreadAccepted = true;
    std::thread wrongThread([&] {
        isWrongThreadAccepted = service.SendReloadUpdate();
    });
    wrongThread.join();
    SetExpect(!isWrongThreadAccepted
            && service.GetMainProp() == firstProp,
        "reload from a non-owner thread must not touch the VTK pipeline",
        failureCount);
    auto replacementRenderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto replacementWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    replacementWindow->SetOffScreenRendering(1);
    replacementWindow->AddRenderer(
        replacementRenderer);
    std::thread wrongBindThread([&] {
        service.SetRenderContext(
            replacementWindow,
            replacementRenderer);
    });
    wrongBindThread.join();
    SetExpect(service.SendReloadUpdate()
            && service.GetMainProp() == firstProp,
        "a non-owner thread must not replace the RenderContext owner",
        failureCount);
    service.SetVizMode(VizMode::IsoSurface);
    SetExpect(service.SendReloadUpdate(),
        "input swap test should cache a second mode",
        failureCount);
    vtkWeakPointer<vtkProp3D> retiredProp =
        service.GetMainProp();
    service.SetVizMode(VizMode::Volume);
    SetExpect(service.SendReloadUpdate()
            && service.GetMainProp() == firstProp,
        "input swap test should return to the cached current mode",
        failureCount);
    auto cropEffect =
        std::make_shared<CropShaderEffect>();
    SetExpect(service.AttachRenderEffect(cropEffect),
        "input swap test should attach one crop effect",
        failureCount);

    CropOpItem keepOp;
    keepOp.operationIndex = 1;
    keepOp.geometryType = CropShape::Plane;
    keepOp.removalMode = CropRemovalMode::KeepInside;
    CropOpItem removeOp = keepOp;
    removeOp.operationIndex = 2;
    removeOp.removalMode =
        CropRemovalMode::RemoveInside;
    const auto tableResult =
        CropAlgorithm::BuildPredicateTable(
            { keepOp, removeOp }, 2);
    CropShaderPayload payload;
    payload.revision = 1;
    payload.sourceStamp =
        service.GetRenderInputStamp();
    payload.nodeCount = 2;
    payload.predicateTable =
        tableResult.predicateTable;
    SetExpect(tableResult.isSucceeded
            && cropEffect->SetCropParams(payload),
        "KeepInside and RemoveInside should stage before input replacement",
        failureCount);

    auto nextImage =
        vtkSmartPointer<vtkImageData>::New();
    nextImage->DeepCopy(firstImage);
    auto mask =
        vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(nextImage);
    mask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    auto* maskValues =
        static_cast<unsigned char*>(
            mask->GetScalarPointer());
    std::fill_n(
        maskValues,
        mask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    const auto expected =
        dataManager->GetSnapshot();
    ImageState candidate = *expected;
    candidate.image = nextImage;
    candidate.validityMask = mask;
    ImageSnapshot published;
    SetExpect(dataManager->SetCandidate(
            std::move(candidate),
            expected,
            published),
        "next image and mask should publish",
        failureCount);
    SetExpect(!service.SendReloadUpdate(),
        "input replacement must wait for a staged crop revision",
        failureCount);
    SetExpect(service.GetMainProp() == firstProp
            && cropEffect->GetState().status
                != RenderEffectStatus::Failed,
        "deferred input replacement must keep the current crop binding valid",
        failureCount);
    SetExpect(cropEffect->ClearCropStage(
            payload.revision),
        "input swap test should finish the staged transaction",
        failureCount);
    std::size_t prewarmCount = 0;
    auto renderCallback =
        vtkSmartPointer<vtkCallbackCommand>::New();
    renderCallback->SetClientData(&prewarmCount);
    renderCallback->SetCallback(
        [](vtkObject*, unsigned long eventId,
            void* clientData, void*) {
            if (eventId == vtkCommand::StartEvent
                && clientData) {
                ++(*static_cast<std::size_t*>(clientData));
            }
        });
    const unsigned long renderTag =
        renderWindow->AddObserver(
            vtkCommand::StartEvent, renderCallback);
    const bool isInputRebuilt =
        service.SendReloadUpdate();
    renderWindow->RemoveObserver(renderTag);
    renderCallback->SetClientData(nullptr);
    std::cout
        << "DIAG_RENDER_SOURCE: candidate_prewarm="
        << prewarmCount << '\n';
    SetExpect(isInputRebuilt
            && prewarmCount >= 1
            && prewarmCount <= 2,
        "same-mode input should use one or two candidate prewarm renders",
        failureCount);
    auto* nextProp = service.GetMainProp();
    SetExpect(firstProp
            && nextProp
            && firstProp != nextProp,
        "same-mode input replacement must swap a prepared strategy instead of mutating the visible strategy",
        failureCount);
    SetExpect(!retiredProp,
        "input replacement must release inactive strategies that retain the previous materialized image",
        failureCount);
}

void StartRenderOwnerGate(int& failureCount)
{
    auto dataManager =
        std::make_shared<DataManagerProbe>();
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(
        static_cast<float*>(image->GetScalarPointer()),
        image->GetNumberOfPoints(), 1.0F);
    SetExpect(dataManager->SetInitial(image),
        "owner gate needs an initial image",
        failureCount);

    auto broadcaster =
        std::make_shared<SharedStateBroadcaster>();
    auto state =
        std::make_shared<SharedInteractionState>(
            broadcaster);
    VizService service(
        dataManager, state, broadcaster);
    auto strategy = std::make_shared<VolumeStrategy>();
    auto overlay = std::make_shared<VolumeStrategy>();
    strategy->SetInputData(image);
    overlay->SetInputData(image);
    service.SetCurrentStrategy(strategy);
    service.AttachOverlayStrategy(overlay);

    auto* volume = vtkVolume::SafeDownCast(
        strategy->GetMainProp());
    auto* property = volume ? volume->GetProperty() : nullptr;
    if (!property) {
        SetExpect(false,
            "owner gate needs a volume property",
            failureCount);
        return;
    }
    const double initialDiffuse = property->GetDiffuse();
    auto material = state->GetMaterial();
    material.diffuse = initialDiffuse == 0.23 ? 0.41 : 0.23;
    service.SetMaterial(material);

    service.SendUpdates();
    SetExpect(std::abs(property->GetDiffuse()
                - initialDiffuse) < 1e-12
            && !service.SendReloadUpdate(),
        "an unbound service must not submit state to VTK",
        failureCount);

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    service.SetRenderContext(renderWindow, renderer);
    service.SendUpdates();
    SetExpect(std::abs(property->GetDiffuse()
                - material.diffuse) < 1e-12,
        "the RenderContext owner must submit pending state",
        failureCount);
}
}

int AppTaskSuite::GetFailCount() const
{
    int failureCount = 0;
    StartVolumeTypes(failureCount);
    StartOwningTasks(failureCount);
    StartExportSnapshot(failureCount);
    StartExportFiles(failureCount);
    StartStateGate(failureCount);
    StartMaskSnapshot(failureCount);
    StartRenderOwnerGate(failureCount);
    StartInputSwap(failureCount);
    return failureCount;
}
