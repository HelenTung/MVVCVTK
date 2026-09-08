// 测试用途：声明正交裁剪算法、着色器预览和应用任务测试套件。
#pragma once

class CropShaderPreviewSuite final {
public:
    int GetFailCount() const;
};

class AppTaskSuite final {
public:
    int GetFailCount() const;
};

class CropAlgorithmSuite final {
public:
    int GetFailCount() const;
};
