#pragma once

#include "OrthogonalCropTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

class InteractiveService;
class vtkRenderWindowInteractor;
class vtkRenderer;

struct CropViewRequest final {
    vtkRenderWindowInteractor* interactor = nullptr;
    vtkRenderer* renderer = nullptr;
    std::shared_ptr<InteractiveService> referenceService;
    std::vector<std::shared_ptr<InteractiveService>> targetServices;
};

using CropExportCallback = std::function<void(CropExportResult)>;

class CropBridge final {
public:
    CropBridge();
    ~CropBridge();

    CropBridge(const CropBridge&) = delete;
    CropBridge& operator=(const CropBridge&) = delete;

    bool StartView(const CropViewRequest& request);
    bool ClearBindings();
    bool SetCropInput(CropInputSnapshot input);
    // DataManager 发布前只准备 history 候选；baseNodeCount=0 表示显式恢复原始基线。
    bool StartCropBaseline(
        CropInputSnapshot input,
        std::size_t baseNodeCount);
    // Start 成功且外部快照发布后调用；完成阶段只移动已准备状态，不允许失败。
    bool SetCropBaselineComplete() noexcept;
    bool ClearCropBaseline();
    bool SwitchCropBox();
    bool SwitchCropPlane();
    bool SetCropMode(CropRemovalMode removalMode);
    bool PreviousCrop();
    bool NextCrop();
    bool SetCropNode(std::size_t nodeCount);
    bool ExitCrop();
    bool GetCropActive() const;
    CropHistoryState GetCropHistory() const;

    bool GetShaderTickNeeded() const;
    bool SendShaderCommit();
    bool ExportCrop(CropExportCallback onComplete);
    bool GetExportTickNeeded() const;
    bool SendExportResult();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
