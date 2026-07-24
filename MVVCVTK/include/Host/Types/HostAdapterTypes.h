#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <string>

struct HostHotkeyConfig {
    // context 输入负责窗口内工具切换；command 输入负责主体数据、孔隙和退出命令。
    bool isContextInputEnabled = false;
    HostViewTargets contextInputViews;
    bool isCommandInputEnabled = false;
    HostViewTargets commandInputViews;
    char modelSwitchKey = 0;
    char saveTransformedDataKey = 0;
    char saveSliceImagesKey = 0;
    char gapSwitchKey = 0;
    std::string exitKeySym; // 使用 VTK key symbol，支持 Escape 等非字符键。
};

struct HostHotkeyTemplates {
    // 热键只选择动作；路径、算法参数和目标窗口均从这些模板复制，避免输入层制造业务默认值。
    HostGapStartRequest gapStart;
    HostVolumeExportRequest volumeExportRequest;
    HostSliceExportRequest sliceExportRequest;
};

struct HostTimerConfig {
    bool isTimerEnabled = false; // false 表示卸载当前 host timer handler。
    HostViewTarget targetView{ "", false, HostRenderViewRole::Primary3D };
};
