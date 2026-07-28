#pragma once

#include "Host/Types/HostValueTypes.h"

#include <string>

struct HostHotkeyConfig {
    // context 输入负责窗口内工具切换；command 输入负责数据动作和退出命令。
    bool isContextInputEnabled = false;
    HostViewTargets contextInputViews;
    bool isCommandInputEnabled = false;
    HostViewTargets commandInputViews;
    char modelSwitchKey = 0;
    char dataExportKey = 0;
    char sliceExportKey = 0;
    std::string exitKeySym; // 使用 VTK key symbol，支持 Escape 等非字符键。
};

struct HostTimerConfig {
    bool isTimerEnabled = false; // false 表示卸载当前 host timer handler。
    HostViewTarget targetView{ "", false, HostRenderViewRole::Primary3D };
};
