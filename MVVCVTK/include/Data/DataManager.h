#pragma once
#include "AppInterfaces.h"
#include <vector>
#include <mutex>
#include <array>
#include <atomic>

struct ReconBuffer {
    std::vector<float>    data;                // 拷贝自重建输出的体素数据
    std::array<int, 3>    dims = { 0, 0, 0 };
    std::array<float, 3>  spacing = { 1.f, 1.f, 1.f };
    std::array<float, 3>  origin = { 0.f, 0.f, 0.f }; // 上游 ITK/LPS 物理原点，提交到 VTK 前统一转 RAS
};


class BaseDataManager : public AbstractDataManager
{
protected:
	mutable std::mutex m_dataMutex;
	vtkSmartPointer<vtkImageData> m_vtkImage;
    std::array<double, 2> m_scalarRange = { 0.0, 0.0 };      // 缓存当前体数据标量范围，避免重复全量扫描
    std::array<double, 3> m_imageSpacing = { 1.0, 1.0, 1.0 }; // 缓存当前体数据 spacing，加载后优先复用
    std::string m_loadedFilePath;

    void SetLoadedFilePath(const std::string& filePath);
public:
    BaseDataManager() {
        m_vtkImage = vtkSmartPointer<vtkImageData>::New();
    }

    vtkSmartPointer<vtkImageData> GetVtkImage() const override {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        return m_vtkImage;
	}

    std::array<double, 2> GetScalarRange() const override;
    std::array<double, 3> GetSpacing() const override;

    bool SaveTransformedData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix) override;
    bool SaveSliceImages(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) override;
    std::string GetDefaultTransformedDataPath() const override;
};

class RawVolumeDataManager : public BaseDataManager {
private:
    int m_dims[3] = { 0, 0, 0 }; // 当前已提交体数据的体素维度
    std::array<double, 3> m_origin = { 0.0, 0.0, 0.0 }; // 当前体数据 RAS 原点，和 spacing 一起定义统一世界坐标映射
    std::array<double, 3> m_spacing = { 1.0, 1.0, 1.0 }; // 空数据使用中性 spacing；真实 spacing 必须由加载/重建命令提交

    // ── 重建注入路径（前后处理分离）──────────────────────────────────
    mutable std::mutex            m_reconMutex; // 保护“后台准备中但尚未提交”的重建快照
    vtkSmartPointer<vtkImageData> m_pendingImage;   // 后台线程写，主线程读
    std::array<double, 2>         m_pendingScalarRange = { 0.0, 0.0 }; // 缓存待提交重建数据范围，主线程消费时直接复用
    std::array<double, 3>         m_pendingSpacing = { 1.0, 1.0, 1.0 }; // 缓存待提交重建数据 spacing，避免再次访问 VTK
    std::atomic<bool>             m_hasPendingImage{ false }; // 单比特提交门铃：后台线程只声明“有新镜像可切换”，主线程据此安全接管 vtkImageData

public:
    RawVolumeDataManager() = default;
    bool SetDataLoaded(const std::string& filePath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
    bool TakeImageSnapshot(vtkSmartPointer<vtkImageData> image);
    bool ConsumePendingImage() override;
};

class TiffVolumeDataManager : public BaseDataManager {
public:
    TiffVolumeDataManager() = default;
    // 实现加载接口
    bool SetDataLoaded(const std::string& filePath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) override;
};
