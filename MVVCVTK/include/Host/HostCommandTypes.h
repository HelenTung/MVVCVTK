#pragma once

#include "Host/HostSessionTypes.h"

#include <functional>
#include <variant>

using HostCompleteCallback = std::function<void(bool isSuccess)>;

struct HostLoadCommand {
    InitialVolumeLoadConfig request;
    HostCompleteCallback onComplete;
};

struct HostReloadCommand {
    HostVolumeBufferRequest request;
    HostCompleteCallback onComplete;
};

struct HostExportCommand {
    HostDataExportConfig request;
    HostCompleteCallback onComplete;
};

struct HostViewCommand {
    HostViewConfig request;
};

enum class HostCropAction {
    Start,
    Box,
    Plane,
    Preview,
    Submit,
    Exit
};

struct HostCropCommand {
    HostCropAction action = HostCropAction::Start;
    HostCropViewRequest request;
    HostCropPreviewMode previewMode = HostCropPreviewMode::KeepInside;
};

enum class HostGapAction {
    Start,
    Switch,
    Overlay,
    Exit
};

struct HostGapCommand {
    HostGapAction action = HostGapAction::Start;
    HostGapViewRequest request;
};

struct HostExitCommand {};

using HostFeatureCommand = std::variant<
    HostCropCommand,
    HostGapCommand,
    HostExitCommand>;

using HostCommand = std::variant<
    std::monostate,
    HostLoadCommand,
    HostReloadCommand,
    HostExportCommand,
    HostViewCommand,
    HostFeatureCommand>;
