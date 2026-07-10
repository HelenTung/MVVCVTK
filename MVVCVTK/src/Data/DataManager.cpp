#include "DataManager.h"
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <vtkTIFFReader.h>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vtkStringArray.h>
#include <vtkTransform.h>
#include <vtkImageReslice.h>
#include <vtkImageChangeInformation.h>
#include <vtkPNGWriter.h>
#include <vtkImageImport.h>
#include <cstring>
#include "MemMappedFile.h"

namespace {

std::array<double, 3> GetRasOrigin(
    const std::array<double, 3>& lpsOrigin,
    const int dims[3],
    const std::array<double, 3>& spacing)
{
    std::array<double, 3> rasOrigin = {
        -lpsOrigin[0],
        -lpsOrigin[1],
        lpsOrigin[2]
    };

    if (dims[0] > 0) {
        rasOrigin[0] -= static_cast<double>(dims[0] - 1) * spacing[0];
    }
    if (dims[1] > 0) {
        rasOrigin[1] -= static_cast<double>(dims[1] - 1) * spacing[1];
    }

    return rasOrigin;
}

}

class BaseDataManager::Impl final {
public:
    Impl()
        : m_vtkImage(vtkSmartPointer<vtkImageData>::New())
    {
    }

    mutable std::mutex m_dataMutex;
    vtkSmartPointer<vtkImageData> m_vtkImage;
    std::array<double, 2> m_scalarRange = { 0.0, 0.0 };
    std::array<double, 3> m_imageSpacing = { 1.0, 1.0, 1.0 };
    DataVersion m_dataVersion = 0;

    bool SetRasScalars(
        const float* src,
        float* dst,
        const int dims[3],
        size_t availableCount) const
    {
        const size_t nx = dims[0] > 0 ? static_cast<size_t>(dims[0]) : 0;
        const size_t ny = dims[1] > 0 ? static_cast<size_t>(dims[1]) : 0;
        const size_t nz = dims[2] > 0 ? static_cast<size_t>(dims[2]) : 0;
        const size_t sliceSize = nx * ny;
        const size_t totalCount = sliceSize * nz;

        if (!dst || totalCount == 0) {
            return false;
        }

        if (!src || availableCount == 0) {
            std::fill(dst, dst + totalCount, 0.0f);
            return true;
        }

        if (availableCount == totalCount) {
            for (size_t z = 0; z < nz; ++z) {
                const size_t srcSliceOffset = z * sliceSize;
                const size_t dstSliceOffset = z * sliceSize;
                for (size_t y = 0; y < ny; ++y) {
                    const float* srcRow = src + srcSliceOffset + y * nx;
                    float* dstRow = dst + dstSliceOffset + (ny - 1 - y) * nx;
                    for (size_t x = 0; x < nx; ++x) {
                        dstRow[nx - 1 - x] = srcRow[x];
                    }
                }
            }
            return true;
        }

        std::fill(dst, dst + totalCount, 0.0f);
        for (size_t srcIndex = 0; srcIndex < availableCount; ++srcIndex) {
            const size_t z = srcIndex / sliceSize;
            const size_t rem = srcIndex % sliceSize;
            const size_t y = rem / nx;
            const size_t x = rem % nx;
            const size_t dstIndex = z * sliceSize + (ny - 1 - y) * nx + (nx - 1 - x);
            dst[dstIndex] = src[srcIndex];
        }

        return true;
    }

    std::string GetOrientName(Orientation value) const
    {
        switch (value) {
        case Orientation::Front_back:
            return "Front_back";
        case Orientation::Left_right:
            return "Left_right";
        case Orientation::Top_down:
        default:
            return "Top_down";
        }
    }

    std::array<int, 2> GetSliceSize(
        const int sliceDims[3],
        Orientation value) const
    {
        switch (value) {
        case Orientation::Front_back:
            return { sliceDims[0], sliceDims[2] };
        case Orientation::Left_right:
            return { sliceDims[1], sliceDims[2] };
        case Orientation::Top_down:
        default:
            return { sliceDims[0], sliceDims[1] };
        }
    }

    unsigned char GetWindowGray(
        double value,
        const WindowLevelParams& params) const
    {
        const double safeWindowWidth = std::max(params.windowWidth, 1e-6); // 当前导出使用的窗宽，避免除零
        const double windowMin = params.windowCenter - safeWindowWidth * 0.5; // 当前灰度映射下限
        const double normalized = (value - windowMin) / safeWindowWidth;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        return static_cast<unsigned char>(clamped * 255.0 + 0.5);
    }
};

class TiffVolumeDataManager::Impl final {
public:
    vtkSmartPointer<vtkImageData> LoadImage(
        const std::string& inputPath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin);

private:
    bool GetExplicitValue(const std::array<float, 3>& values) const;
    bool SetLpsRasImage(vtkImageData* source, vtkImageData* target) const;
};

class RawVolumeDataManager::Impl final {
public:
    mutable std::mutex m_reconMutex; // 保护后台准备、门铃发布和主线程接管这一完整事务
    vtkSmartPointer<vtkImageData> m_pendingImage;
    std::array<double, 2> m_pendingScalarRange = { 0.0, 0.0 };
    std::array<double, 3> m_pendingSpacing = { 1.0, 1.0, 1.0 };
    bool m_hasPendingImage = false; // 只在 m_reconMutex 内读写，不再需要独立原子状态
};

BaseDataManager::BaseDataManager()
    : m_impl(std::make_unique<BaseDataManager::Impl>())
{
}

BaseDataManager::~BaseDataManager() = default;

vtkSmartPointer<vtkImageData> BaseDataManager::GetVtkImage() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_vtkImage;
}

std::array<double, 2> BaseDataManager::GetScalarRange() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_scalarRange;
}

std::array<double, 3> BaseDataManager::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_imageSpacing;
}

DataVersion BaseDataManager::GetDataVersion() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_dataVersion;
}

ImageState BaseDataManager::GetImageState() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return { m_impl->m_vtkImage, m_impl->m_dataVersion };
}

bool BaseDataManager::SetCurrentImage(vtkSmartPointer<vtkImageData> image)
{
    if (!image) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    double imageSpacing[3] = { 1.0, 1.0, 1.0 };
    image->GetScalarRange(range);
    image->GetSpacing(imageSpacing);

    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        m_impl->m_vtkImage = std::move(image);
        m_impl->m_scalarRange = { range[0], range[1] };
        m_impl->m_imageSpacing = { imageSpacing[0], imageSpacing[1], imageSpacing[2] };
        ++m_impl->m_dataVersion;
    }

    return true;
}

bool BaseDataManager::ExportSlices(
    const std::string& dirPath,
    Orientation orientation,
    const WindowLevelParams& windowLevel,
    const std::array<double, 16>& modelToWorldMatrix)
{

    if (dirPath.empty()) {
        std::cerr << "[Export] Slice image export failed: output directory is empty." << std::endl;
        return false;
    }

    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        if (!m_impl->m_vtkImage) return false;
        imageCopy->ShallowCopy(m_impl->m_vtkImage);
    }

    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(modelToWorldMatrix.data());
    worldToModelMatrix->Invert();

    auto worldToModelTransform = vtkSmartPointer<vtkTransform>::New();
    worldToModelTransform->SetMatrix(worldToModelMatrix);

    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(worldToModelTransform);
    reslice->SetInterpolationModeToLinear();
    reslice->SetOutputDimensionality(3);
    reslice->SetAutoCropOutput(true);

    double range[2] = { 0.0, 0.0 };
    imageCopy->GetScalarRange(range);
    reslice->SetBackgroundLevel(range[0]);

    try {
        reslice->Update();
    }
    catch (...) {
        return false;
    }

    auto outputImage = reslice->GetOutput();
    if (!outputImage || outputImage->GetNumberOfPoints() == 0) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    outputImage->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    std::filesystem::path outputDir = std::filesystem::path(dirPath);
    if (outputDir.has_extension()) {
        outputDir = outputDir.parent_path() / outputDir.stem();
    }
    if (outputDir.empty()) {
        return false;
    }

    try {
        std::filesystem::create_directories(outputDir);
    }
    catch (...) {
        return false;
    }

    const int sliceAxis = static_cast<int>(orientation); // 与 Orientation 枚举值保持一致
    const int sliceCount = dims[sliceAxis];
    const std::array<int, 2> sliceSize = m_impl->GetSliceSize(dims, orientation); // 当前导出图片宽高
    const int width = sliceSize[0];
    const int height = sliceSize[1];
    const int digits = std::max(4, static_cast<int>(std::to_string(std::max(sliceCount - 1, 0)).size()));
    const std::string orientationName = m_impl->GetOrientName(orientation);

    for (int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        auto sliceImage = vtkSmartPointer<vtkImageData>::New();
        sliceImage->SetDimensions(width, height, 1);
        sliceImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

        auto* dst = static_cast<unsigned char*>(sliceImage->GetScalarPointer());
        if (!dst) {
            return false;
        }
        for (int py = 0; py < height; ++py) {
            for (int px = 0; px < width; ++px) {
                int x = 0;
                int y = 0;
                int z = 0;

                if (orientation == Orientation::Top_down) {
                    x = px;
                    y = py;
                    z = sliceIndex;
                }
                else if (orientation == Orientation::Front_back) {
                    x = px;
                    y = sliceIndex;
                    z = py;
                }
                else {
                    x = sliceIndex;
                    y = px;
                    z = py;
                }
                const double value = outputImage->GetScalarComponentAsDouble(x, y, z, 0);
                dst[py * width + px] = m_impl->GetWindowGray(value, windowLevel);
            }
        }

        std::ostringstream fileName;
        fileName << orientationName << "_"
            << std::setw(digits) << std::setfill('0') << sliceIndex
            << ".png";

        auto writer = vtkSmartPointer<vtkPNGWriter>::New();
        const std::filesystem::path outputFilePath = outputDir / fileName.str();
        const std::string vtkFileName = outputFilePath.u8string();
        writer->SetFileName(vtkFileName.c_str());
        writer->SetInputData(sliceImage);
        writer->Write();
        if (writer->GetErrorCode() != 0) {
            return false;
        }
    }

    return true;
}

bool BaseDataManager::ExportData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix)
{
    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        if (!m_impl->m_vtkImage) return false;
        // 浅拷贝数据指针，避免在主线程引发锁竞争
        imageCopy->ShallowCopy(m_impl->m_vtkImage);
    }

    //  VTK 逆变换矩阵
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToModelMatrix->DeepCopy(modelToWorldMatrix.data());
    worldToModelMatrix->Invert();

    auto worldToModelTransform = vtkSmartPointer<vtkTransform>::New();
    worldToModelTransform->SetMatrix(worldToModelMatrix);

    // 利用 vtkImageReslice 进行核心插值运算
    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(worldToModelTransform);
    reslice->SetInterpolationModeToLinear();

    // VTK 会自动计算旋转后新的 Bounding Box，避免模型的边角被切割。
    // 这会导致输出的数据维度（Dimensions）发生变化。
    reslice->SetOutputDimensionality(3);

    reslice->SetAutoCropOutput(true);
    double range[2];
    imageCopy->GetScalarRange(range);
    reslice->SetBackgroundLevel(range[0]); // 取真实的最小标量值

    try {
        // 更新管线，触发计算
        reslice->Update();
    }
    catch (...) {
        std::cerr << "[Error] Exception during image reslicing/changing info." << std::endl;
        return false;
    }

    auto outputImage = reslice->GetOutput();
    if (!outputImage || outputImage->GetNumberOfPoints() == 0) {
        std::cerr << "[Error] Reslice produced an empty image!" << std::endl;
        return false;
    }

    // 提取维度大小和内存指针
    int newDims[3];
    outputImage->GetDimensions(newDims);
    float* outDataPtr = static_cast<float*>(outputImage->GetScalarPointer());

    if (!outDataPtr) {
        return false;
    }

    vtkIdType incs[3];
    outputImage->GetIncrements(incs);

    std::filesystem::path pathObj(filePath);
    std::string dimStr = std::to_string(newDims[0]) + "X" + std::to_string(newDims[1]) + "X" + std::to_string(newDims[2]);
    // 组合：所在目录 / (原文件名无后缀 + "_" + 维度 + 原后缀)
    std::filesystem::path finalFileName = pathObj.stem();
    finalFileName += std::string("_") + dimStr;
    finalFileName += pathObj.extension();
    std::filesystem::path finalPath = pathObj.parent_path() / finalFileName;

    // 使用 C++ 标准库直接持久化写入裸 raw 数据
    std::ofstream rawFile(finalPath, std::ios::binary);
    if (!rawFile.is_open()) {
        std::cerr << "[Error] Failed to open RAW file for writing: " << filePath << std::endl;
        return false;
    }

    // 由于旋转后行尾可能有 Padding（根据 VTK 内部内存布局），不能直接写入整块内存。
    size_t rowBytes = static_cast<size_t>(newDims[0]) * sizeof(float);
    for (int z = 0; z < newDims[2]; ++z) {
        for (int y = 0; y < newDims[1]; ++y) {
            // 利用真实步长计算出当前行准确的内存起始地址
            float* rowPtr = outDataPtr + z * incs[2] + y * incs[1];
            // 每次只写入当前行真正有效的数据宽度（摒弃行尾的 Padding）
            rawFile.write(reinterpret_cast<const char*>(rowPtr), rowBytes);
        }
    }

    rawFile.close();
    std::cout << "[Export] Successfully saved transformed RAW to: " << finalPath.u8string() << "\n"
        << "[Export] IMPORTANT: New Dimensions are "
        << newDims[0] << " x " << newDims[1] << " x " << newDims[2] << std::endl;

    return true;
}

RawVolumeDataManager::RawVolumeDataManager()
    : m_rawImpl(std::make_unique<RawVolumeDataManager::Impl>())
{
}

RawVolumeDataManager::~RawVolumeDataManager() = default;

bool RawVolumeDataManager::SetDataLoaded(const std::string& filePath,
    const std::array<float, 3>& spacing, const std::array<float, 3>& origin) {
    // 解析文件名
    std::filesystem::path pathObj(filePath);
    std::string name = pathObj.filename().string();
    std::regex pattern(R"((\d+)[xX](\d+)[xX](\d+))");
    std::smatch matches;

    int newDims[3] = { 0, 0, 0 };
    if (std::regex_search(name, matches, pattern) && matches.size() > 3) {
        newDims[0] = std::stoi(matches[1].str());
        newDims[1] = std::stoi(matches[2].str());
        newDims[2] = std::stoi(matches[3].str());
    }
    else {
        return false;
    }

    // --- 先释放旧内存，再分配新内存 ---
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        if (m_impl->m_vtkImage) {
            m_impl->m_vtkImage = nullptr;     // 置空防止悬垂指针
        }
    }

	// 打开文件统一把 ITK/LPS 物理坐标转换成 VTK 侧统一使用的 RAS 物理坐标。
    const std::array<double, 3> lpsSpacing = {
        static_cast<double>(spacing[0]),
        static_cast<double>(spacing[1]),
        static_cast<double>(spacing[2])
    };
    const std::array<double, 3> lpsOrigin = {
        static_cast<double>(origin[0]),
        static_cast<double>(origin[1]),
        static_cast<double>(origin[2])
    };
    const std::array<double, 3> rasSpacing = lpsSpacing;
    const std::array<double, 3> rasOrigin = GetRasOrigin(lpsOrigin, newDims, rasSpacing);

    // 创建全新的 vtkImageData 对象
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(newDims[0], newDims[1], newDims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    // 读取数据到新内存
    size_t totalVoxels = static_cast<size_t>(newDims[0]) * static_cast<size_t>(newDims[1]) * static_cast<size_t>(newDims[2]);
    size_t expectedBytes = totalVoxels * sizeof(float);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());

    // 使用 MemMappedFile 替代 std::ifstream读取
    MemMappedFile mmf;
    if (mmf.Load(filePath)) {
        //   - 文件够大 → 只取前 expectedBytes
        //   - 文件偏小 → 只拷文件实际字节，剩余保持 AllocateScalars 的零值
        size_t copyBytes = (mmf.GetSize() < expectedBytes) ? mmf.GetSize() : expectedBytes;
        const size_t copyCount = copyBytes / sizeof(float);

        if (mmf.GetSize() < expectedBytes) {
            std::cerr << "[Warn] File size (" << mmf.GetSize()
                << ") < expected (" << expectedBytes
                << "). Partial load, remainder zeroed." << std::endl;
        }

        if (!m_impl->SetRasScalars(
            reinterpret_cast<const float*>(mmf.GetData()),
            dst,
            newDims,
            copyCount)) {
            return false;
        }
        // mmf 析构时自动 close()
    }
    else {
        // mmap 打开失败，fallback 到原始 ifstream
        std::cerr << "[Warn] MemMappedFile open failed, fallback to ifstream: "
            << filePath << std::endl;
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }
        std::vector<float> srcBuffer(totalVoxels, 0.0f);
        file.read(reinterpret_cast<char*>(srcBuffer.data()),
            static_cast<std::streamsize>(expectedBytes));
        const size_t readBytes = static_cast<size_t>(file.gcount());
        const size_t readCount = readBytes / sizeof(float);
        if (!m_impl->SetRasScalars(srcBuffer.data(), dst, newDims, readCount)) {
            return false;
        }
        file.close();
    }


    newImage->Modified();
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    // --- 提交  ---
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        m_impl->m_vtkImage = std::move(newImage); // 指针交换，旧数据若被渲染线程持有则暂存，否则释放
        m_impl->m_scalarRange = { range[0], range[1] };
        m_impl->m_imageSpacing = rasSpacing;
        ++m_impl->m_dataVersion;
    }
    return true;
}

bool RawVolumeDataManager::SetFromBuffer(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin)
{
    if (!data || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    // ── 在调用方线程完成唯一一次分配 + 拷贝（只此一次，不再重复）────
	// 重建注入路径与文件打开路径保持一致：统一在这里完成 LPS -> RAS 物理坐标转换。
    // vtkImageImport 解决少一次拷贝的问题，更快的读取速度，更快的读取效率
    const int rasDims[3] = { dims[0], dims[1], dims[2] };
    const std::array<double, 3> rasSpacing = {
        static_cast<double>(spacing[0]),
        static_cast<double>(spacing[1]),
        static_cast<double>(spacing[2])
    };
    const std::array<double, 3> lpsOrigin = {
        static_cast<double>(origin[0]),
        static_cast<double>(origin[1]),
        static_cast<double>(origin[2])
    };
    const std::array<double, 3> rasOrigin = GetRasOrigin(lpsOrigin, rasDims, rasSpacing);
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(dims[0], dims[1], dims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);   // 分配（page-fault 在此触发）
    const size_t total =
        static_cast<size_t>(dims[0]) *
        static_cast<size_t>(dims[1]) *
        static_cast<size_t>(dims[2]);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    if (!m_impl->SetRasScalars(data, dst, rasDims, total)) {
        return false;
    }
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    // Modified() 暂不调用——留到主线程 SetCurrentFromPending() 中调用，
    // 确保 VTK pipeline 脏标记传播在正确线程触发。

    {
        std::lock_guard<std::mutex> lock(m_rawImpl->m_reconMutex);
        m_rawImpl->m_pendingImage = std::move(newImage);   // 指针赋值，无拷贝
        m_rawImpl->m_pendingScalarRange = { range[0], range[1] };
        m_rawImpl->m_pendingSpacing = rasSpacing;
        m_rawImpl->m_hasPendingImage = true;
    }

    return true;
}

bool RawVolumeDataManager::SetImageSnapshot(vtkSmartPointer<vtkImageData> image)
{
    if (!image) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 || !image->GetScalarPointer()) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    image->GetScalarRange(range);
    const std::array<double, 3> imageSpacing = {
        image->GetSpacing()[0],
        image->GetSpacing()[1],
        image->GetSpacing()[2]
    };

    {
        std::lock_guard<std::mutex> lock(m_rawImpl->m_reconMutex);
        m_rawImpl->m_pendingImage = std::move(image);
        m_rawImpl->m_pendingScalarRange = { range[0], range[1] };
        m_rawImpl->m_pendingSpacing = imageSpacing;
        m_rawImpl->m_hasPendingImage = true;
    }

    return true;
}

bool RawVolumeDataManager::SetCurrentFromPending()
{
    vtkSmartPointer<vtkImageData> incoming;
    std::array<double, 2> pendingRange = { 0.0, 0.0 };
    std::array<double, 3> pendingSpacing = { 1.0, 1.0, 1.0 };
    {
        std::lock_guard<std::mutex> lock(m_rawImpl->m_reconMutex);
        // 门铃和 payload 使用同一事务锁，避免锁外检查后后台线程替换 pending image。
        if (!m_rawImpl->m_hasPendingImage || !m_rawImpl->m_pendingImage) {
            return false;
        }
        incoming = std::move(m_rawImpl->m_pendingImage);
        pendingRange = m_rawImpl->m_pendingScalarRange;
        pendingSpacing = m_rawImpl->m_pendingSpacing;
        // 先在互斥区内拿走所有权，再清掉门铃，保证主线程这一帧只消费一次这批重建结果。
        m_rawImpl->m_hasPendingImage = false;
    }

    // Modified() 在主线程调用（VTK pipeline 脏传播安全）
    incoming->Modified();

    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        m_impl->m_vtkImage = incoming;
        m_impl->m_scalarRange = pendingRange;
        m_impl->m_imageSpacing = pendingSpacing;
        ++m_impl->m_dataVersion;
    }
    // 到这里新镜像已经成为当前唯一真源，调用方随后再通过 SharedState 发布 DataReady，
    // Service 层就能沿着“DataReady -> 请求重建 -> 下一帧消费”的链路继续推进。
    return true;
}

TiffVolumeDataManager::TiffVolumeDataManager()
    : m_impl(std::make_unique<TiffVolumeDataManager::Impl>())
{
}

TiffVolumeDataManager::~TiffVolumeDataManager() = default;

bool TiffVolumeDataManager::SetDataLoaded(
    const std::string& inputPath,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin)
{
    if (!m_impl) {
        return false;
    }

    auto image = m_impl->LoadImage(inputPath, spacing, origin);
    return SetCurrentImage(std::move(image));
}

vtkSmartPointer<vtkImageData> TiffVolumeDataManager::Impl::LoadImage(
    const std::string& inputPath,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin) {
    // 路径检查
    std::filesystem::path pathObj(inputPath);
    if (!std::filesystem::exists(pathObj)) {
        std::cerr << "[Error] Path does not exist: " << inputPath << std::endl;
        return nullptr;
    }

    auto reader = vtkSmartPointer<vtkTIFFReader>::New();


    if (std::filesystem::is_directory(pathObj)) {
        //
        std::cout << "[Info] Loading TIFF series from folder: " << inputPath << std::endl;

        std::vector<std::string> fileList;

        // 遍历文件夹
        for (const auto& entry : std::filesystem::directory_iterator(pathObj)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                // 转小写比较，兼容 .TIF 和 .tif
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".tif" || ext == ".tiff") {
                    fileList.push_back(entry.path().string());
                }
            }
        }

        if (fileList.empty()) {
            std::cerr << "[Error] No .tif files found in folder." << std::endl;
            return nullptr;
        }

        auto naturalSort = [](const std::string& s1, const std::string& s2) {
            size_t i = 0, j = 0;
            while (i < s1.size() && j < s2.size()) {
                // 如果两边当前字符都是数字，提取整个数字进行数值比较
                if (std::isdigit(s1[i]) && std::isdigit(s2[j])) {
                    unsigned long long n1 = 0;
                    unsigned long long n2 = 0;

                    // 解析 s1 中的数字
                    while (i < s1.size() && std::isdigit(s1[i])) {
                        n1 = n1 * 10 + (s1[i] - '0');
                        i++;
                    }
                    // 解析 s2 中的数字
                    while (j < s2.size() && std::isdigit(s2[j])) {
                        n2 = n2 * 10 + (s2[j] - '0');
                        j++;
                    }

                    if (n1 != n2) {
                        return n1 < n2; // 数值小的在前
                    }
                    // 如果数值相等 (例如 01 和 1)，继续比较后续字符
                }
                else {
                    // 非数字字符，按标准 ASCII 比较
                    // (如果需要忽略大小写，可在此处转 tolower)
                    if (s1[i] != s2[j]) {
                        return s1[i] < s2[j];
                    }
                    i++;
                    j++;
                }
            }
            // 如果一个字符串是另一个的前缀，短的在前
            return s1.size() < s2.size();
        };


        // 排序
        std::sort(fileList.begin(), fileList.end(), naturalSort);

        // 构造 VTK 字符串数组
        auto vtkFiles = vtkSmartPointer<vtkStringArray>::New();
        for (const auto& f : fileList) {
            vtkFiles->InsertNextValue(f);
        }

        // 设置文件列表
        reader->SetFileNames(vtkFiles);
    }
    else {
        // 单文件
        std::cout << "[Info] Loading single TIFF file: " << inputPath << std::endl;
        reader->SetFileName(inputPath.c_str());

        if (!reader->CanReadFile(inputPath.c_str())) {
            std::cerr << "[Error] VTK cannot read this TIFF file." << std::endl;
            return nullptr;
        }
    }

    // Reader 会自动处理 Origin 和 Spacing (如果有的话)
    // 如果是序列图片，Reader 会根据文件数量自动设置 Z 轴维度
    try {
        reader->Update();
    }
    catch (...) {
        std::cerr << "[Error] Exception during TIFF reading." << std::endl;
        return nullptr;
    }

    auto output = reader->GetOutput();
    if (!output || output->GetDimensions()[0] == 0) {
        return nullptr;
    }

    // --- 数据提交 (Back Buffer 策略) ---
    auto lpsImage = vtkSmartPointer<vtkImageData>::New();
    lpsImage->ShallowCopy(output);
    if (GetExplicitValue(spacing)) {
        lpsImage->SetSpacing(spacing[0], spacing[1], spacing[2]);
    }
    if (GetExplicitValue(origin)) {
        lpsImage->SetOrigin(origin[0], origin[1], origin[2]);
    }

    auto newImage = vtkSmartPointer<vtkImageData>::New();
    if (!SetLpsRasImage(lpsImage, newImage)) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    newImage->GetDimensions(dims);

    std::cout << "[Success] Loaded Volume: " << dims[0] << "x" << dims[1] << "x" << dims[2] << std::endl;
    return newImage;
}

bool TiffVolumeDataManager::Impl::GetExplicitValue(const std::array<float, 3>& values) const
{
    return std::any_of(values.begin(), values.end(), [](const float value) {
        return value != 0.0f;
    });
}

bool TiffVolumeDataManager::Impl::SetLpsRasImage(
    vtkImageData* source,
    vtkImageData* target) const
{
    if (!source || !target) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    source->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        return false;
    }

    double spacingRaw[3] = { 1.0, 1.0, 1.0 };
    double originRaw[3] = { 0.0, 0.0, 0.0 };
    source->GetSpacing(spacingRaw);
    source->GetOrigin(originRaw);

    const std::array<double, 3> imageSpacing = {
        spacingRaw[0], spacingRaw[1], spacingRaw[2]
    };
    const std::array<double, 3> lpsOrigin = {
        originRaw[0], originRaw[1], originRaw[2]
    };
    const std::array<double, 3> rasOrigin = GetRasOrigin(lpsOrigin, dims, imageSpacing);

    target->SetDimensions(dims[0], dims[1], dims[2]);
    target->SetSpacing(imageSpacing[0], imageSpacing[1], imageSpacing[2]);
    target->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    target->AllocateScalars(source->GetScalarType(), source->GetNumberOfScalarComponents());

    const int componentCount = source->GetNumberOfScalarComponents();
    const int scalarSize = source->GetScalarSize();
    const size_t pixelBytes = static_cast<size_t>(componentCount) * static_cast<size_t>(scalarSize);
    const size_t nx = static_cast<size_t>(dims[0]);
    const size_t ny = static_cast<size_t>(dims[1]);
    const size_t rowBytes = nx * pixelBytes;
    const size_t sliceBytes = ny * rowBytes;

    const char* srcBase = static_cast<const char*>(source->GetScalarPointer());
    char* dstBase = static_cast<char*>(target->GetScalarPointer());
    if (!srcBase || !dstBase) {
        return false;
    }

    for (size_t z = 0; z < static_cast<size_t>(dims[2]); ++z) {
        const size_t srcSliceOffset = z * sliceBytes;
        const size_t dstSliceOffset = z * sliceBytes;
        for (size_t y = 0; y < ny; ++y) {
            const char* srcRow = srcBase + srcSliceOffset + y * rowBytes;
            char* dstRow = dstBase + dstSliceOffset + (ny - 1 - y) * rowBytes;
            for (size_t x = 0; x < nx; ++x) {
                std::memcpy(
                    dstRow + (nx - 1 - x) * pixelBytes,
                    srcRow + x * pixelBytes,
                    pixelBytes);
            }
        }
    }

    target->Modified();
    return true;
}
