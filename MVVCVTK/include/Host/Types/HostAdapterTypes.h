#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <string>

struct HostHotkeyConfig {
    // context 输入负责窗口内工具切换；command 输入负责数据、裁切、孔隙和退出命令，两域可指向不同窗口。
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
    char submitKey = 0; // 提交键还要求 Ctrl 修饰，避免普通字符输入误提交。
    std::string exitKeySym; // 使用 VTK key symbol，支持 Escape 等非字符键。
};

struct HostHotkeyTemplates {
    // 热键只选择动作；路径、算法参数和目标窗口均从这些模板复制，避免输入层制造业务默认值。
    HostCropTargetRequest cropTarget;
    HostGapStartRequest gapStart;
    HostVolumeExportRequest volumeExportRequest;
    HostSliceExportRequest sliceExportRequest;
};

struct HostTimerConfig {
    bool isTimerEnabled = false; // false 表示卸载当前 host timer handler。
    HostViewTarget targetView{ "", false, HostRenderViewRole::Primary3D };
};
