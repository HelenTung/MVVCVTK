#pragma once

#include "Host/Types/HostValueTypes.h"

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct HostVolumeGeometry {
    std::array<int, 3> dimensions{ 0, 0, 0 };
    std::array<float, 3> spacing{};
    std::array<float, 3> origin{};
};

enum class HostCropPreviewMode { KeepInside, RemoveInside };
enum class HostGapAnalysisIsoMode { DataRangeRatio, AbsoluteValue };

enum class HostDataAction { None, LoadFile, ReloadBuffer, ExportVolume, ExportSlices };

struct HostLoadRequest {
    std::string filePath;
    HostVolumeGeometry geometry;
};

struct HostReloadRequest {
    std::vector<float> voxels;
    HostVolumeGeometry geometry;
};

struct HostVolumeExportRequest { std::string outputPath; };

struct HostSliceExportRequest {
    std::string outputDir;
    HostViewTarget sourceView{ "", false, HostRenderViewRole::TopDownSlice };
    std::optional<double> angleDeg;
};

using HostDataPayload = std::variant<std::monostate, HostLoadRequest,
    HostReloadRequest, HostVolumeExportRequest, HostSliceExportRequest>;

struct HostDataRequest {
    HostDataAction action = HostDataAction::None;
    HostDataPayload payload;
};

struct HostViewSetRequest {
    HostViewTarget targetView;
    std::optional<HostViewMode> mode;
    std::optional<HostMaterialParams> material;
    std::optional<double> opacity;
    std::optional<std::vector<HostTransferNode>> transferNodes;
    std::optional<double> iso;
    std::optional<HostBackgroundColor> background;
    std::optional<std::array<double, 3>> spacing;
    std::optional<HostWindowLevelParams> windowLevel;
};

enum class HostViewAction { None, Set };
using HostViewPayload = std::variant<std::monostate, HostViewSetRequest>;
struct HostViewRequest {
    HostViewAction action = HostViewAction::None;
    HostViewPayload payload;
};

struct HostToolSetRequest {
    HostViewTarget targetView;
    HostToolMode toolMode = HostToolMode::Navigation;
};

struct HostToolSwitchRequest { HostViewTarget targetView; };
enum class HostToolAction { None, Set, Switch };
using HostToolPayload = std::variant<std::monostate, HostToolSetRequest, HostToolSwitchRequest>;
struct HostToolRequest {
    HostToolAction action = HostToolAction::None;
    HostToolPayload payload;
};

struct HostCropTargetRequest {
    HostViewTarget referenceView{ "", false, HostRenderViewRole::Primary3D };
    HostViewTargets previewViews;
    bool isPreviewViewsUsed = false;
};

struct HostCropPreviewRequest {
    HostCropTargetRequest target;
    HostCropPreviewMode previewMode = HostCropPreviewMode::KeepInside;
};

enum class HostCropAction { None, Start, Box, Plane, Preview, Submit, Exit };
using HostCropPayload = std::variant<std::monostate, HostCropTargetRequest, HostCropPreviewRequest>;
struct HostCropRequest {
    HostCropAction action = HostCropAction::None;
    HostCropPayload payload;
};

struct HostGapSurfaceConfig {
    HostGapAnalysisIsoMode isoMode = HostGapAnalysisIsoMode::DataRangeRatio;
    double dataRangeRatio = 0.0;
    double absoluteIsoValue = 0.0;
};

struct HostGapVoidConfig {
    float grayMin = 0.0f;
    float grayMax = 0.0f;
    float minVolumeMM3 = 0.0f;
    float angleThresholdDeg = 0.0f;
    int tensorWindowSize = 0;
    int erosionIterations = 0;
};

struct HostGapConfig {
    HostGapSurfaceConfig surface;
    HostGapVoidConfig voidDetection;
};

struct HostGapStartRequest {
    HostViewTargets targetViews;
    bool isDefaultOverlayUsed = false;
    HostGapConfig algorithm;
};

enum class HostGapAction { None, Start, Overlay, Exit };
using HostGapPayload = std::variant<std::monostate, HostGapStartRequest>;
struct HostGapRequest {
    HostGapAction action = HostGapAction::None;
    HostGapPayload payload;
};
