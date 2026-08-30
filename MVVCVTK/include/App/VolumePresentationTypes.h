#pragma once

#include <cstdint>
#include <vector>

// App/Render 内部唯一传递函数模型；横坐标始终为数据真实 scalar。
struct VolumeTransferFunction final {
    struct ColorNode final {
        double scalar = 0.0;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };

    struct OpacityNode final {
        double scalar = 0.0;
        double opacity = 0.0;
    };

    std::vector<ColorNode> colorNodes;
    std::vector<OpacityNode> opacityNodes;
};

// 只保存产品质量意图；比例、采样与资源策略由 VolumeLodController 持有。
enum class VolumeQuality : std::uint8_t {
    Auto,
    Low,
    High,
    XHigh,
    Ultra
};

struct GradientOpacityNode final {
    double gradient = 0.0; // VTK gradient-opacity 原生域中的梯度幅值，必须非负。
    double opacity = 0.0;  // 归一化不透明度 [0,1]。
};
