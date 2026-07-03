#pragma once

#include <array>
#include <memory>
#include <string>

// 当前类只表达“独立 VTK 宿主如何组装一次应用会话”：
// 1. main 只启动会话，避免后续上位机协议继续堆进入口函数。
// 2. 具体热键属于 host/session 配置；feature 插件只暴露命令能力，不保存固定键位。
class VtkAppHostSession final {
public:
    struct HostHotkeyBindings {
        char modelTransformToggleKey = 'm';
        char saveTransformedDataKey = 's';
        char saveSliceImagesKey = 't';
        char cropToggleKey = 'o';
        char planarCropToggleKey = 'p';
        char gapOverlayToggleKey = 'j';
        char keepInsidePreviewKey = '1';
        char removeInsidePreviewKey = '2';
        char submitKey = '3';
        std::string exitKeySym = "Escape";
    };

    struct InitialVolumeLoadConfig {
        std::string filePath = "F:\\data\\1000x1000x1000.raw";
        std::array<float, 3> spacing = { 0.02125f, 0.02125f, 0.02125f };
        std::array<float, 3> origin = { 0.0f, 0.0f, 0.0f };
        int histogramBinCount = 2048;
        std::string histogramFilePath = "";
    };

    struct Config {
        HostHotkeyBindings hotkeys;
        InitialVolumeLoadConfig initialVolume;
    };

    VtkAppHostSession();
    explicit VtkAppHostSession(Config config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    void Initialize();
    void Start();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
