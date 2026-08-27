#pragma once

// App 与 Interaction 共享的稳定视图语义；该头不依赖 App 状态、服务或 VTK。
enum class VizMode {
    Volume,
    IsoSurface,
    SliceTop_down,
    SliceFront_back,
    SliceLeft_right,
    CompositeVolume,        // 3D 体渲染 + 切片平面
    CompositeIsoSurface     // 3D 等值面 + 切片平面
};

enum class ToolMode {
    Navigation,         // 默认漫游/切片浏览
    ModelTransform      // 模型变换（旋转/缩放/平移）
};
