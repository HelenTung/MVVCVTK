#include "DataManager.h"
#include "Platform/Path.h"
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <fstream>
#include <filesystem>
#include <vtkTIFFReader.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <vtkTransform.h>
#include <vtkImageReslice.h>
#include <vtkImageChangeInformation.h>
#include <vtkPNGWriter.h>
#include <vtkImageImport.h>
#include <vtkImageAppend.h>
#include <cstring>
#include "MemMappedFile.h"

class BaseDataManager::Impl final {
public:
    Impl()
        : m_current(std::make_shared<ImageState>(ImageState{
            vtkSmartPointer<vtkImageData>::New(),
            { 0, 0, 0 },
            { 1.0, 1.0, 1.0 },
            { 0.0, 0.0, 0.0 },
            { 0.0, 0.0 },
            0 }))
    {
    }

    static std::array<double, 3> GetRasOrigin(
        const std::array<double, 3>& lpsOrigin,
        const int dims[3],
        const std::array<double, 3>& spacing)
    {
        std::array<double, 3> rasOrigin = {
            -lpsOrigin[0], -lpsOrigin[1], lpsOrigin[2]
        };
        if (dims[0] > 0) {
            rasOrigin[0] -= static_cast<double>(dims[0] - 1) * spacing[0];
        }
        if (dims[1] > 0) {
            rasOrigin[1] -= static_cast<double>(dims[1] - 1) * spacing[1];
        }
        return rasOrigin;
    }

    static std::optional<size_t> GetVoxelCount(const int dims[3])
    {
        size_t voxelCount = 1;
        for (int axis = 0; axis < 3; ++axis) {
            if (dims[axis] <= 0
                || voxelCount > std::numeric_limits<size_t>::max()
                    / static_cast<size_t>(dims[axis])) {
                return std::nullopt;
            }
            voxelCount *= static_cast<size_t>(dims[axis]);
        }
        if (voxelCount > std::numeric_limits<size_t>::max() / sizeof(float)
            || voxelCount > static_cast<size_t>(
                std::numeric_limits<std::ptrdiff_t>::max())) {
            return std::nullopt;
        }
        return voxelCount;
    }

    bool SetCurrent(ImageState state)
    {
        if (!state.image) {
            return false;
        }
        auto nextState = std::make_shared<ImageState>(std::move(state));
        std::shared_ptr<const ImageState> retiredState;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (!m_current
                || m_current->version == std::numeric_limits<DataVersion>::max()) {
                return false;
            }
            nextState->version = m_current->version + 1;
            retiredState = std::move(m_current);
            m_current = std::move(nextState);
        }
        return true;
    }

    // current ImageState 与 scalar range 共用此锁；snapshot 是跨字段一致性的读取入口。
    mutable std::mutex m_dataMutex;
    // current 只向受控内部消费链发布 const owner；写入只能通过 DataManager 提交新批次。
    ImageSnapshot m_current;
    mutable std::mutex m_pendingMutex;
    ImageState m_pending{};
    // 与 current image 同批提交的 RAS 物理轴间距 [x,y,z]，单位沿用输入。

    bool SetRasScalars(
        const float* src,
        float* dst,
        const int dims[3],
        size_t availableCount) const
    {
        const auto voxelCount = GetVoxelCount(dims);
        if (!voxelCount) {
            return false;
        }
        const size_t nx = static_cast<size_t>(dims[0]);
        const size_t ny = static_cast<size_t>(dims[1]);
        const size_t nz = static_cast<size_t>(dims[2]);
        const size_t sliceSize = nx * ny;
        const size_t totalCount = *voxelCount;

        if (!dst || totalCount == 0 || availableCount > totalCount) {
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
        const VolumeLayout& layout);

private:
    bool SetLpsRasImage(vtkImageData* source, vtkImageData* target) const;
};

BaseDataManager::BaseDataManager()
    : m_impl(std::make_unique<BaseDataManager::Impl>())
{
}

BaseDataManager::~BaseDataManager() = default;

vtkSmartPointer<vtkImageData> BaseDataManager::GetVtkImage() const
{
    return GetImageState().image;
}

std::array<double, 2> BaseDataManager::GetScalarRange() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->scalarRange;
}

std::array<double, 3> BaseDataManager::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->spacing;
}

bool BaseDataManager::SetSpacing(const std::array<double, 3>& spacing)
{
    // 1. spacing 是物理尺度，三个分量都必须有限且严格为正。
    if (!std::all_of(spacing.begin(), spacing.end(), [](double value) {
        return std::isfinite(value) && value > 0.0;
        })) {
        return false;
    }

    constexpr int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        // 2. 锁内取得 immutable current 身份，锁外构造候选批次，避免复制/VTK 调用阻塞读线程。
        std::shared_ptr<const ImageState> baseState;
        {
            std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
            if (!m_impl->m_current || !m_impl->m_current->image) {
                return false;
            }
            if (m_impl->m_current->spacing == spacing) {
                return true;
            }
            if (m_impl->m_current->version == std::numeric_limits<DataVersion>::max()) {
                return false;
            }
            baseState = m_impl->m_current;
        }

        // spacing 只修改 VTK 外壳；scalar 在版本间保持只读共享，避免复制整卷 voxel。
        auto candidate = vtkSmartPointer<vtkImageData>::New();
        candidate->ShallowCopy(baseState->image);
        candidate->SetSpacing(spacing.data());

        auto nextState = std::make_shared<ImageState>(*baseState);
        nextState->image = std::move(candidate);
        nextState->spacing = spacing;
        nextState->version = baseState->version + 1;
        std::shared_ptr<const ImageState> retiredState;
        {
            std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
            // 3. 仅当 current 仍是基准对象才提交；并发发布抢先时重试，三次冲突后返回失败。
            if (m_impl->m_current != baseState) {
                continue;
            }
            retiredState = std::move(m_impl->m_current);
            m_impl->m_current = std::move(nextState);
        }
        return true;
    }
    return false;
}

DataVersion BaseDataManager::GetDataVersion() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current->version;
}

ImageState BaseDataManager::GetImageState() const
{
    const auto currentState = GetImageSnapshot();
    ImageState publicState = *currentState;
    if (publicState.image) {
        auto imageCopy = vtkSmartPointer<vtkImageData>::New();
        imageCopy->DeepCopy(publicState.image);
        publicState.image = std::move(imageCopy);
    }
    return publicState;
}

ImageSnapshot BaseDataManager::GetImageSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
    return m_impl->m_current;
}

bool BaseDataManager::SetCurrentData(
    const ImageSnapshot& snapshot,
    DataVersion expectedVersion)
{
    if (!snapshot || !snapshot->image
        || expectedVersion == std::numeric_limits<DataVersion>::max()) {
        return false;
    }

    // snapshot 的 image/metadata 是 immutable owner；恢复只复制轻量状态并共享体素，不做整卷复制。
    auto nextState = std::make_shared<ImageState>(*snapshot);
    std::shared_ptr<const ImageState> retiredState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        // 只撤销自己刚发布的版本；若已有更新事务抢先，绝不覆盖更晚 current。
        if (!m_impl->m_current
            || m_impl->m_current->version != expectedVersion) {
            return false;
        }
        nextState->version = expectedVersion + 1;
        retiredState = std::move(m_impl->m_current);
        m_impl->m_current = std::move(nextState);
    }
    return true;
}

bool BaseDataManager::SetFromBuffer(
    const VolumeBuffer&)
{
    return false;
}

bool BaseDataManager::SetCurrentFromPending(bool& hasPending)
{
    // pending 是单槽 staging：锁内一次性 take，锁外调用 VTK Modified/发布 current。
    // 这样 worker 只准备候选数据，主线程提交时不会跨 pending 锁进入 VTK。
    hasPending = false;
    ImageState incoming;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
        if (!m_impl->m_pending.image) return true;
        hasPending = true;
        incoming = std::move(m_impl->m_pending);
        m_impl->m_pending = {};
    }
    incoming.image->Modified();
    return m_impl->SetCurrent(std::move(incoming));
}

bool BaseDataManager::ClearPending()
{
    ImageState retiredPending;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
        // 锁内只断开 pending 引用；旧候选在离开锁后析构，避免释放大体数据拉长临界区。
        retiredPending = std::move(m_impl->m_pending);
        m_impl->m_pending = {};
    }
    return true;
}

bool BaseDataManager::SetPendingImage(ImageState image)
{
    if (!image.image) return false;
    ImageState retiredPending;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_pendingMutex);
        // 单槽只保留最新候选；被覆盖批次在离开锁后随 retiredPending 析构。
        retiredPending = std::move(m_impl->m_pending);
        m_impl->m_pending = std::move(image);
    }
    return true;
}

bool BaseDataManager::SetOwnedImage(vtkSmartPointer<vtkImageData> image)
{
    if (!image) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    double imageSpacing[3] = { 1.0, 1.0, 1.0 };
    double imageOrigin[3] = { 0.0, 0.0, 0.0 };
    image->GetScalarRange(range);
    image->GetSpacing(imageSpacing);
    image->GetOrigin(imageOrigin);

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return m_impl->SetCurrent({
        std::move(image),
        { dims[0], dims[1], dims[2] },
        { imageSpacing[0], imageSpacing[1], imageSpacing[2] },
        { imageOrigin[0], imageOrigin[1], imageOrigin[2] },
        { range[0], range[1] },
        0
    });
}

bool BaseDataManager::ExportSlices(
    const std::string& dirPath,
    Orientation orientation,
    const WindowLevelParams& windowLevel,
    const std::array<double, 16>& modelToWorldMatrix)
{

    // 导出路径：1. 固定 current 批次并把 modelToWorld 取逆；2. 重采样到轴对齐体数据；
    // 3. 按 Orientation 将二维像素映射回 X/Y/Z；4. 应用窗宽窗位并逐层写 PNG。

    if (dirPath.empty()) {
        std::cerr << "[Export] Slice image export failed: output directory is empty." << std::endl;
        return false;
    }

    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    std::shared_ptr<const ImageState> currentState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        currentState = m_impl->m_current;
    }
    if (!currentState->image) return false;
    imageCopy->ShallowCopy(currentState->image);

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

    std::filesystem::path outputDir = PlatformPath::GetNativePath(dirPath);
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

                // A. TopDown 固定 Z，图片横纵轴映射为 X/Y。
                if (orientation == Orientation::Top_down) {
                    x = px;
                    y = py;
                    z = sliceIndex;
                }
                // B. FrontBack 固定 Y，图片横纵轴映射为 X/Z。
                else if (orientation == Orientation::Front_back) {
                    x = px;
                    y = sliceIndex;
                    z = py;
                }
                // C. LeftRight 固定 X，图片横纵轴映射为 Y/Z。
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
        const std::string vtkFileName = PlatformPath::GetUtf8Path(outputFilePath);
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
    // RAW 导出路径：固定 immutable current -> 逆变换重采样 -> 自动裁剪新 bounds ->
    // 按 VTK increments 逐行剥离 padding，最终写出无头、X-fast 的 float32 数据。
    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    std::shared_ptr<const ImageState> currentState;
    {
        std::lock_guard<std::mutex> lock(m_impl->m_dataMutex);
        currentState = m_impl->m_current;
    }
    if (!currentState->image) return false;
    // 私有 current 批次不可变，因此可在锁外建立只读浅拷贝供导出管线使用。
    imageCopy->ShallowCopy(currentState->image);

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

    // RAW 契约固定为单分量 float32；其它标量布局必须显式拒绝，不能按 float* 误解释。
    if (outputImage->GetScalarType() != VTK_FLOAT
        || outputImage->GetNumberOfScalarComponents() != 1) {
        return false;
    }
    int newDims[3];
    outputImage->GetDimensions(newDims);
    float* outDataPtr = static_cast<float*>(outputImage->GetScalarPointer());

    if (!outDataPtr) {
        return false;
    }

    vtkIdType incs[3];
    outputImage->GetIncrements(incs);

    std::filesystem::path pathObj = PlatformPath::GetNativePath(filePath);
    std::string dimStr = std::to_string(newDims[0]) + "X" + std::to_string(newDims[1]) + "X" + std::to_string(newDims[2]);
    // 组合：所在目录 / (原文件名无后缀 + "_" + 维度 + 原后缀)
    std::filesystem::path finalFileName = pathObj.stem();
    finalFileName += std::string("_") + dimStr;
    finalFileName += pathObj.extension();
    std::filesystem::path finalPath = pathObj.parent_path() / finalFileName;

    // 按 x-fast、逐行无 padding 的 float32 裸数据写出，不附带维度、spacing、origin 等元数据。
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
            if (!rawFile) {
                return false;
            }
        }
    }

    rawFile.close();
    if (!rawFile) {
        return false;
    }
    std::cout << "[Export] Successfully saved transformed RAW to: "
        << PlatformPath::GetUtf8Path(finalPath) << "\n"
        << "[Export] IMPORTANT: New Dimensions are "
        << newDims[0] << " x " << newDims[1] << " x " << newDims[2] << std::endl;

    return true;
}

RawVolumeDataManager::RawVolumeDataManager()
{
}

RawVolumeDataManager::~RawVolumeDataManager() = default;

bool RawVolumeDataManager::SetDataLoaded(
    const std::string& filePath,
    const VolumeLayout& layout)
{
    const auto& dimensions = layout.GetDimensions();
    const int rasDims[3] = {
        dimensions[0], dimensions[1], dimensions[2]
    };
    std::error_code fileError;
    const auto fileBytes = std::filesystem::file_size(
        PlatformPath::GetNativePath(filePath), fileError);
    if (fileError || fileBytes != layout.GetByteCount()) {
        return false;
    }

    // 输入 layout 明确描述 LPS 物理空间；加载链只负责转换为内部 RAS 空间。
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
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
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(
        lpsOrigin, rasDims, rasSpacing);

    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(rasDims[0], rasDims[1], rasDims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    MemMappedFile mmf;
    if (!mmf.Load(filePath) || mmf.GetSize() != layout.GetByteCount()
        || !m_impl->SetRasScalars(
            static_cast<const float*>(mmf.GetData()), dst,
            rasDims, layout.GetVoxelCount())) {
        return false;
    }

    newImage->Modified();
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    return SetPendingImage({
        std::move(newImage),
        dimensions,
        rasSpacing,
        rasOrigin,
        { range[0], range[1] },
        0
    });
}

bool RawVolumeDataManager::SetFromBuffer(
    const VolumeBuffer& buffer)
{
    const auto& layout = buffer.GetLayout();
    const auto& dims = layout.GetDimensions();
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
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
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(
        lpsOrigin, rasDims, rasSpacing);
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(dims[0], dims[1], dims[2]);
    newImage->SetSpacing(rasSpacing[0], rasSpacing[1], rasSpacing[2]);
    newImage->SetOrigin(rasOrigin[0], rasOrigin[1], rasOrigin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    if (!m_impl->SetRasScalars(
        buffer.GetVoxels().data(), dst, rasDims, layout.GetVoxelCount())) {
        return false;
    }
    double range[2] = { 0.0, 0.0 };
    newImage->GetScalarRange(range);

    return SetPendingImage({
        std::move(newImage), dims, rasSpacing, rasOrigin,
        { range[0], range[1] }, 0 });
}

bool RawVolumeDataManager::SetImageSnapshot(vtkSmartPointer<vtkImageData> image)
{
    // 该入口接收已构造的 VTK image，只校验并发布 pending 快照，不直接替换 current 真源。
    if (!image) {
        return false;
    }

    auto imageCopy = vtkSmartPointer<vtkImageData>::New();
    imageCopy->DeepCopy(image);

    int dims[3] = { 0, 0, 0 };
    imageCopy->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0 || !imageCopy->GetScalarPointer()) {
        return false;
    }

    double range[2] = { 0.0, 0.0 };
    imageCopy->GetScalarRange(range);
    const std::array<double, 3> imageSpacing = {
        imageCopy->GetSpacing()[0],
        imageCopy->GetSpacing()[1],
        imageCopy->GetSpacing()[2]
    };
    const std::array<double, 3> imageOrigin = {
        imageCopy->GetOrigin()[0],
        imageCopy->GetOrigin()[1],
        imageCopy->GetOrigin()[2]
    };

    return SetPendingImage({
        std::move(imageCopy),
        { dims[0], dims[1], dims[2] },
        imageSpacing,
        imageOrigin,
        { range[0], range[1] },
        0 });
}

bool RawVolumeDataManager::SetCurrentFromPending(bool& hasPending)
{
    return BaseDataManager::SetCurrentFromPending(hasPending);
}

bool RawVolumeDataManager::ClearPending()
{
    return BaseDataManager::ClearPending();
}

TiffVolumeDataManager::TiffVolumeDataManager()
    : m_impl(std::make_unique<TiffVolumeDataManager::Impl>())
{
}

TiffVolumeDataManager::~TiffVolumeDataManager() = default;

bool TiffVolumeDataManager::SetDataLoaded(
    const std::string& inputPath,
    const VolumeLayout& layout)
{
    if (!m_impl) {
        return false;
    }

    auto image = m_impl->LoadImage(inputPath, layout);
    if (!image) return false;
    double range[2] = { 0.0, 0.0 };
    image->GetScalarRange(range);
    const auto& dimensions = layout.GetDimensions();
    double spacing[3] = { 1.0, 1.0, 1.0 };
    double origin[3] = { 0.0, 0.0, 0.0 };
    image->GetSpacing(spacing);
    image->GetOrigin(origin);
    return SetPendingImage({
        std::move(image), dimensions,
        { spacing[0], spacing[1], spacing[2] },
        { origin[0], origin[1], origin[2] },
        { range[0], range[1] }, 0 });
}

vtkSmartPointer<vtkImageData> TiffVolumeDataManager::Impl::LoadImage(
    const std::string& inputPath,
    const VolumeLayout& layout) {
    // 路径检查
    const std::filesystem::path pathObj = PlatformPath::GetNativePath(inputPath);
    if (!std::filesystem::exists(pathObj)) {
        std::cerr << "[Error] Path does not exist: " << inputPath << std::endl;
        return nullptr;
    }

    vtkSmartPointer<vtkImageData> decodedImage;
    if (std::filesystem::is_directory(pathObj)) {
        //
        std::cout << "[Info] Loading TIFF series from folder: " << inputPath << std::endl;

        std::vector<std::string> fileList;

        try {
            for (const auto& entry : std::filesystem::directory_iterator(pathObj)) {
                if (entry.is_regular_file()) {
                    std::string ext = PlatformPath::GetUtf8Path(entry.path().extension());
                    // 转小写比较，兼容 .TIF 和 .tif
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

                    if (ext == ".tif" || ext == ".tiff") {
                        fileList.push_back(PlatformPath::GetUtf8Path(entry.path()));
                    }
                }
            }
        }
        catch (...) {
            std::cerr << "[Error] Failed to enumerate TIFF directory." << std::endl;
            return nullptr;
        }

        if (fileList.empty()) {
            std::cerr << "[Error] No .tif files found in folder." << std::endl;
            return nullptr;
        }

        auto naturalSort = [](const std::string& s1, const std::string& s2) {
            size_t i = 0, j = 0;
            const auto isDigit = [](char value) {
                return std::isdigit(static_cast<unsigned char>(value)) != 0;
            };
            while (i < s1.size() && j < s2.size()) {
                if (isDigit(s1[i]) && isDigit(s2[j])) {
                    const size_t digitStart1 = i;
                    const size_t digitStart2 = j;
                    while (i < s1.size() && isDigit(s1[i])) {
                        ++i;
                    }
                    while (j < s2.size() && isDigit(s2[j])) {
                        ++j;
                    }
                    size_t significant1 = digitStart1;
                    size_t significant2 = digitStart2;
                    while (significant1 < i && s1[significant1] == '0') {
                        ++significant1;
                    }
                    while (significant2 < j && s2[significant2] == '0') {
                        ++significant2;
                    }
                    const size_t length1 = i - significant1;
                    const size_t length2 = j - significant2;
                    if (length1 != length2) {
                        return length1 < length2;
                    }
                    const int digitCompare = s1.compare(
                        significant1, length1, s2, significant2, length2);
                    if (digitCompare != 0) {
                        return digitCompare < 0;
                    }
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

        // 每张 TIFF 独立解码后沿 Z 轴组装，避开批量 SetFileNames 在部分 Windows
        // libtiff 构建中的跨文件状态损坏；fileList 的自然排序仍是切片顺序真源。
        auto append = vtkSmartPointer<vtkImageAppend>::New();
        append->SetAppendAxis(2);
        std::vector<vtkSmartPointer<vtkImageData>> slices;
        slices.reserve(fileList.size());
        int expectedExtent[6] = { 0, -1, 0, -1, 0, -1 };
        int expectedScalarType = VTK_VOID;
        int expectedComponents = 0;
        try {
            for (const auto& file : fileList) {
                auto sliceReader = vtkSmartPointer<vtkTIFFReader>::New();
                sliceReader->SetFileName(file.c_str());
                if (!sliceReader->CanReadFile(file.c_str())) {
                    return nullptr;
                }
                sliceReader->Update();
                if (sliceReader->GetErrorCode() != 0
                    || !sliceReader->GetOutput()
                    || sliceReader->GetOutput()->GetNumberOfPoints() == 0) {
                    return nullptr;
                }
                auto slice = vtkSmartPointer<vtkImageData>::New();
                slice->DeepCopy(sliceReader->GetOutput());
                int sliceExtent[6] = { 0, -1, 0, -1, 0, -1 };
                slice->GetExtent(sliceExtent);
                if (slice->GetDimensions()[0] <= 0
                    || slice->GetDimensions()[1] <= 0
                    || slice->GetDimensions()[2] != 1) {
                    return nullptr;
                }
                if (slices.empty()) {
                    std::copy(std::begin(sliceExtent), std::end(sliceExtent), expectedExtent);
                    expectedScalarType = slice->GetScalarType();
                    expectedComponents = slice->GetNumberOfScalarComponents();
                }
                else if (sliceExtent[0] != expectedExtent[0]
                    || sliceExtent[1] != expectedExtent[1]
                    || sliceExtent[2] != expectedExtent[2]
                    || sliceExtent[3] != expectedExtent[3]
                    || slice->GetScalarType() != expectedScalarType
                    || slice->GetNumberOfScalarComponents() != expectedComponents) {
                    return nullptr;
                }
                slices.push_back(slice);
                append->AddInputData(slice);
            }
            append->Update();
        }
        catch (...) {
            std::cerr << "[Error] Exception during TIFF series reading." << std::endl;
            return nullptr;
        }
        if (append->GetErrorCode() != 0
            || !append->GetOutput()
            || append->GetOutput()->GetNumberOfPoints() == 0) {
            return nullptr;
        }
        decodedImage = vtkSmartPointer<vtkImageData>::New();
        decodedImage->DeepCopy(append->GetOutput());
    }
    else {
        // 单文件
        std::cout << "[Info] Loading single TIFF file: " << inputPath << std::endl;
        auto reader = vtkSmartPointer<vtkTIFFReader>::New();
        const std::string vtkInputPath = PlatformPath::GetUtf8Path(pathObj);
        reader->SetFileName(vtkInputPath.c_str());

        if (!reader->CanReadFile(vtkInputPath.c_str())) {
            std::cerr << "[Error] VTK cannot read this TIFF file." << std::endl;
            return nullptr;
        }
        try {
            reader->Update();
            if (reader->GetErrorCode() != 0
                || !reader->GetOutput()
                || reader->GetOutput()->GetNumberOfPoints() == 0) {
                return nullptr;
            }
        }
        catch (...) {
            std::cerr << "[Error] Exception during TIFF reading." << std::endl;
            return nullptr;
        }
        decodedImage = vtkSmartPointer<vtkImageData>::New();
        decodedImage->DeepCopy(reader->GetOutput());
    }

    auto output = decodedImage;
    if (!output || output->GetDimensions()[0] == 0) {
        return nullptr;
    }

    const auto& expectedDims = layout.GetDimensions();
    const int* decodedDims = output->GetDimensions();
    if (decodedDims[0] != expectedDims[0]
        || decodedDims[1] != expectedDims[1]
        || decodedDims[2] != expectedDims[2]) {
        return nullptr;
    }

    // --- 数据提交 (Back Buffer 策略) ---
    auto lpsImage = vtkSmartPointer<vtkImageData>::New();
    lpsImage->ShallowCopy(output);
    const auto& spacing = layout.GetSpacing();
    const auto& origin = layout.GetOrigin();
    lpsImage->SetSpacing(spacing[0], spacing[1], spacing[2]);
    lpsImage->SetOrigin(origin[0], origin[1], origin[2]);

    auto newImage = vtkSmartPointer<vtkImageData>::New();
    if (!SetLpsRasImage(lpsImage, newImage)) {
        return nullptr;
    }

    int dims[3] = { 0, 0, 0 };
    newImage->GetDimensions(dims);

    std::cout << "[Success] Loaded Volume: " << dims[0] << "x" << dims[1] << "x" << dims[2] << std::endl;
    return newImage;
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
    const std::array<double, 3> rasOrigin = BaseDataManager::Impl::GetRasOrigin(lpsOrigin, dims, imageSpacing);

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
