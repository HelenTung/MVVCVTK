#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <string>

struct HostHotkeyConfig {
    bool isContextInputEnabled = false;
    HostViewTargets contextInputViews;
    bool isCommandInputEnabled = false;
    HostViewTargets commandInputViews;
    char modelSwitchKey = 0;
    char saveTransformedDataKey = 0;
    char saveSliceImagesKey = 0;
    char cropSwitchKey = 0;
    char planarSwitchKey = 0;
    char gapSwitchKey = 0;
    char keepInsidePreviewKey = 0;
    char removeInsidePreviewKey = 0;
    char submitKey = 0;
    std::string exitKeySym;
};

struct HostHotkeyTemplates {
    HostCropTargetRequest cropTarget;
    HostGapStartRequest gapStart;
    HostVolumeExportRequest volumeExportRequest;
    HostSliceExportRequest sliceExportRequest;
};

struct HostTimerConfig {
    bool isTimerEnabled = false;
    HostViewTarget targetView{ "", false, HostRenderViewRole::Primary3D };
};
