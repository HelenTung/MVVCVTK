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
#include <sstream>
#include <vtkStringArray.h> 
#include <vtkTransform.h>
#include <vtkImageReslice.h>
#include <vtkImageChangeInformation.h>
#include <vtkPNGWriter.h>
#include <vtkImageImport.h>
#include "MemMappedFile.h"


namespace {
std::array<double, 2> GetBufferScalarRange(const float* data, size_t count)
{
    if (!data || count == 0) {
        return { 0.0, 0.0 };
    }

    float minValue = data[0];
    float maxValue = data[0];
    for (size_t i = 1; i < count; ++i) {
        const float value = data[i];
        if (value < minValue) minValue = value;
        if (value > maxValue) maxValue = value;
    }

    return { static_cast<double>(minValue), static_cast<double>(maxValue) };
}

std::array<double, 2> GetCopiedScalarRange(const float* src, float* dst, size_t count)
{
    if (!src || !dst || count == 0) {
        return { 0.0, 0.0 };
    }

    float minValue = src[0];
    float maxValue = src[0];
    for (size_t i = 0; i < count; ++i) {
        const float value = src[i];
        dst[i] = value;
        if (value < minValue) minValue = value;
        if (value > maxValue) maxValue = value;
    }

    return { static_cast<double>(minValue), static_cast<double>(maxValue) };
}

std::string GetOrientationName(const Orientation orientation)
{
    switch (orientation) {
    case Orientation::Front_back:
        return "Front_back";
    case Orientation::Left_right:
        return "Left_right";
    case Orientation::Top_down:
    default:
        return "Top_down";
    }
}

std::array<int, 2> GetSliceImageSize(const int dims[3], const Orientation orientation)
{
    switch (orientation) {
    case Orientation::Front_back:
        return { dims[0], dims[2] };
    case Orientation::Left_right:
        return { dims[1], dims[2] };
    case Orientation::Top_down:
    default:
        return { dims[0], dims[1] };
    }
}

bool HasExplicitValue(const std::array<float, 3>& values)
{
    return std::any_of(values.begin(), values.end(), [](const float value) {
        return value != 0.0f;
    });
}

unsigned char GetWindowLevelGray(const double value, const WindowLevelParams& windowLevel)
{
    const double safeWindowWidth = std::max(windowLevel.windowWidth, 1e-6); // 当前导出使用的窗宽，避免除零
    const double windowMin = windowLevel.windowCenter - safeWindowWidth * 0.5; // 当前灰度映射下限
    const double normalized = (value - windowMin) / safeWindowWidth;
    const double clamped = std::clamp(normalized, 0.0, 1.0);
    return static_cast<unsigned char>(clamped * 255.0 + 0.5);
}

std::filesystem::path GetDefaultSliceDir(const std::string& loadedFilePath, const Orientation orientation)
{
    const std::string orientationName = GetOrientationName(orientation);
    if (loadedFilePath.empty()) {
        return std::filesystem::current_path() / (std::string("SliceExport_") + orientationName);
    }

    std::filesystem::path loadedPath = std::filesystem::path(loadedFilePath).lexically_normal();
    std::filesystem::path parentPath = loadedPath.has_parent_path()
        ? loadedPath.parent_path()
        : std::filesystem::current_path();

    std::string sourceName = loadedPath.has_extension()
        ? loadedPath.stem().string()
        : loadedPath.filename().string();
    if (sourceName.empty()) {
        sourceName = "Volume";
    }

    return parentPath / (sourceName + "_" + orientationName + "_Slices");
}
}

void BaseDataManager::SetLoadedFilePath(const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_loadedFilePath = filePath;
}

std::array<double, 2> BaseDataManager::GetScalarRange() const
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    return m_scalarRange;
}

std::array<double, 3> BaseDataManager::GetSpacing() const
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    return m_imageSpacing;
}

std::string BaseDataManager::GetDefaultTransformedDataPath() const
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    return m_loadedFilePath;
}

bool RawVolumeDataManager::SetDataLoaded(const std::string& filePath, 
    const std::array<float, 3>& spacing, const std::array<float, 3>& origin) {

    SetLoadState(LoadState::Loading);
    SetLoadedFilePath({});
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
        SetLoadState(LoadState::Failed);
        return false;
    }

    // --- 先释放旧内存，再分配新内存 ---
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        if (m_vtkImage) {
            m_vtkImage = nullptr;     // 置空防止悬垂指针
        }
    }

	// 更新 spacing 和 origin，确保后续重建和渲染使用最新的空间信息
    for (size_t i = 0; i < spacing.size(); i++)
    {
		m_spacing[i] = static_cast<double>(spacing[i]);
    }

    for (size_t i = 0; i < origin.size(); i++)
    {
        m_origin[i] = static_cast<double>(origin[i]);
    }

    // 创建全新的 vtkImageData 对象 
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(newDims[0], newDims[1], newDims[2]);
    newImage->SetSpacing(m_spacing[0], m_spacing[1], m_spacing[2]);
    newImage->SetOrigin(m_origin[0], m_origin[1], m_origin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    // 读取数据到新内存
    size_t totalVoxels = static_cast<size_t>(newDims[0]) * static_cast<size_t>(newDims[1]) * static_cast<size_t>(newDims[2]);
    // float* vtkDataPtr = static_cast<float*>(newImage->GetScalarPointer());
    size_t expectedBytes = totalVoxels * sizeof(float);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());

    // 使用 MemMappedFile 替代 std::ifstream读取
    MemMappedFile mmf;
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    if (mmf.SetOpened(filePath)) {
        //   - 文件够大 → 只取前 expectedBytes
        //   - 文件偏小 → 只拷文件实际字节，剩余保持 AllocateScalars 的零值
        size_t copyBytes = (mmf.GetSize() < expectedBytes) ? mmf.GetSize() : expectedBytes;
        const size_t copyCount = copyBytes / sizeof(float);

        if (mmf.GetSize() < expectedBytes) {
            std::cerr << "[Warn] File size (" << mmf.GetSize()
                << ") < expected (" << expectedBytes
                << "). Partial load, remainder zeroed." << std::endl;
        }

        scalarRange = GetCopiedScalarRange(
            reinterpret_cast<const float*>(mmf.GetData()),
            dst,
            copyCount);
        if (copyCount < totalVoxels) {
            std::fill(dst + copyCount, dst + totalVoxels, 0.0f);
            scalarRange[0] = std::min(scalarRange[0], 0.0);
            scalarRange[1] = std::max(scalarRange[1], 0.0);
        }
        // mmf 析构时自动 close()
    }
    else {
        // mmap 打开失败，fallback 到原始 ifstream
        std::cerr << "[Warn] MemMappedFile open failed, fallback to ifstream: "
            << filePath << std::endl;
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            SetLoadState(LoadState::Failed);
            return false;
        }
        file.read(reinterpret_cast<char*>(dst),
            static_cast<std::streamsize>(expectedBytes));
        const size_t readBytes = static_cast<size_t>(file.gcount());
        const size_t readCount = readBytes / sizeof(float);
        scalarRange = GetBufferScalarRange(dst, readCount);
        if (readCount < totalVoxels) {
            std::fill(dst + readCount, dst + totalVoxels, 0.0f);
            scalarRange[0] = std::min(scalarRange[0], 0.0);
            scalarRange[1] = std::max(scalarRange[1], 0.0);
        }
        file.close();
    }


    newImage->Modified();
 
    // --- 提交  ---
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_vtkImage = std::move(newImage); // 指针交换，旧数据若被渲染线程持有则暂存，否则释放
        m_dims[0] = newDims[0];
        m_dims[1] = newDims[1];
        m_dims[2] = newDims[2];
        m_scalarRange = scalarRange;
        m_imageSpacing = { m_spacing[0], m_spacing[1], m_spacing[2] };
    }
	SetLoadState(LoadState::Succeeded);
    SetLoadedFilePath(filePath);
    return true;
}

bool RawVolumeDataManager::SetFromBuffer(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin)
{
    SetLoadedFilePath({});

    if (!data || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        SetLoadState(LoadState::Failed);
        return false;
    }

    SetLoadState(LoadState::Loading);

    // ── 在调用方线程完成唯一一次分配 + 拷贝（只此一次，不再重复）────
	// 在坐这里做itk转vtk坐标系的统一,LPS 转 RAS（VTK 默认）坐标系，确保后续处理和渲染的一致性。
    // 后面就用TransformIndexToPhysicalPoint这个接口去调用，不要自己算了
    // vtkImageImport 解决少一次拷贝的问题，更快的读取速度，更快的读取效率
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(dims[0], dims[1], dims[2]);
    newImage->SetSpacing(spacing[0], spacing[1], spacing[2]);
    newImage->SetOrigin(origin[0], origin[1], origin[2]);
    newImage->AllocateScalars(VTK_FLOAT, 1);   // 分配（page-fault 在此触发）
    const size_t total =
        static_cast<size_t>(dims[0]) *
        static_cast<size_t>(dims[1]) *
        static_cast<size_t>(dims[2]);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());
    const auto scalarRange = GetCopiedScalarRange(data, dst, total);  // 唯一一次拷贝，同时缓存范围

    // Modified() 暂不调用——留到主线程 ConsumeReconImage() 中调用，
    // 确保 VTK pipeline 脏标记传播在正确线程触发。

    {
        std::lock_guard<std::mutex> lock(m_reconMutex);
        m_pendingImage = std::move(newImage);   // 指针赋值，无拷贝
        m_pendingScalarRange = scalarRange;
		m_spacing = { spacing[0], spacing[1], spacing[2] };
        m_pendingSpacing = { spacing[0], spacing[1], spacing[2] };
    }
    m_hasPendingImage.store(true);

    // LoadState::Succeeded 延迟到主线程 SetReconImageConsumed() 设置，
    // 防止调用方在 vtkImage 还未提交时就查询到 Succeeded 状态。
    return true;
}

bool RawVolumeDataManager::SetReconImageConsumed()
{
    if (!m_hasPendingImage.load()) return false;

    vtkSmartPointer<vtkImageData> incoming;
    {
        std::lock_guard<std::mutex> lock(m_reconMutex);
        if (!m_pendingImage) return false;
        incoming = std::move(m_pendingImage);
        m_hasPendingImage.store(false);
    }

    // Modified() 在主线程调用（VTK pipeline 脏传播安全）
    incoming->Modified();

    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_vtkImage = incoming;
        m_dims[0] = incoming->GetDimensions()[0];
        m_dims[1] = incoming->GetDimensions()[1];
        m_dims[2] = incoming->GetDimensions()[2];
        m_spacing = { incoming->GetSpacing()[0], incoming->GetSpacing()[1], incoming->GetSpacing()[2] };
        m_scalarRange = m_pendingScalarRange;
        m_imageSpacing = m_pendingSpacing;
    }
    SetLoadState(LoadState::Succeeded);

    return true;
}

bool BaseDataManager::SetSliceImagesSaved(
    const std::string& dirPath,
    Orientation orientation,
    const WindowLevelParams& windowLevel,
    const std::array<double, 16>& transformMatrix)
{
    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    std::string loadedFilePath;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        if (!m_vtkImage) return false;
        imageCopy->ShallowCopy(m_vtkImage);
        loadedFilePath = m_loadedFilePath;
    }

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(transformMatrix.data());
    matrix->Invert();

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(transform);
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

    std::filesystem::path outputDir = dirPath.empty()
        ? GetDefaultSliceDir(loadedFilePath, orientation)
        : std::filesystem::path(dirPath);
    if (outputDir.has_extension()) {
        outputDir = outputDir.parent_path() / outputDir.stem();
    }

    try {
        std::filesystem::create_directories(outputDir);
    }
    catch (...) {
        return false;
    }

    const int sliceAxis = static_cast<int>(orientation); // 与 Orientation 枚举值保持一致
    const int sliceCount = dims[sliceAxis];
    const std::array<int, 2> sliceSize = GetSliceImageSize(dims, orientation); // 当前导出图片宽高
    const int width = sliceSize[0];
    const int height = sliceSize[1];
    const int digits = std::max(4, static_cast<int>(std::to_string(std::max(sliceCount - 1, 0)).size()));
    const std::string orientationName = GetOrientationName(orientation);

    for (int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        auto sliceImage = vtkSmartPointer<vtkImageData>::New();
        sliceImage->SetDimensions(width, height, 1);
        sliceImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

        auto* dst = static_cast<unsigned char*>(sliceImage->GetScalarPointer());
        if (!dst) {
            return false;
        }
		// 取裸指针，然后做eigen优化库运算，或者集合openmp 开启 avx2编译指令集加速，以及cuda做并行运算也可以
		// 现在这里做的是最简单的三重循环，效率不高，后续可以优化
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
                dst[py * width + px] = GetWindowLevelGray(value, windowLevel);
            }
        }

        std::ostringstream fileName;
        fileName << orientationName << "_"
            << std::setw(digits) << std::setfill('0') << sliceIndex
            << ".png";

        auto writer = vtkSmartPointer<vtkPNGWriter>::New();
        const std::filesystem::path outputFilePath = outputDir / fileName.str();
        writer->SetFileName(outputFilePath.string().c_str());
        writer->SetInputData(sliceImage);
        writer->Write();
    }

    return true;
}

bool TiffVolumeDataManager::SetDataLoaded(const std::string& inputPath, 
    const std::array<float, 3>& spacing, 
    const std::array<float, 3>& origin) {
    SetLoadState(LoadState::Loading);
    SetLoadedFilePath({});
    // 路径检查
    std::filesystem::path pathObj(inputPath);
    if (!std::filesystem::exists(pathObj)) {
        SetLoadState(LoadState::Failed);
        std::cerr << "[Error] Path does not exist: " << inputPath << std::endl;
        return false;
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
            SetLoadState(LoadState::Failed);
            std::cerr << "[Error] No .tif files found in folder." << std::endl;
            return false;
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
            SetLoadState(LoadState::Failed);
            std::cerr << "[Error] VTK cannot read this TIFF file." << std::endl;
            return false;
        }
    }

    // Reader 会自动处理 Origin 和 Spacing (如果有的话)
    // 如果是序列图片，Reader 会根据文件数量自动设置 Z 轴维度
    try {
        reader->Update();
    }   
    catch (...) {
        SetLoadState(LoadState::Failed);
        std::cerr << "[Error] Exception during TIFF reading." << std::endl;
        return false;
    }

    auto output = reader->GetOutput();
    if (!output || output->GetDimensions()[0] == 0) {
        SetLoadState(LoadState::Failed);
        return false;
    }

    // --- 数据提交 (Back Buffer 策略) ---
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->ShallowCopy(output);
    if (HasExplicitValue(spacing)) {
        newImage->SetSpacing(spacing[0], spacing[1], spacing[2]);
    }
    if (HasExplicitValue(origin)) {
        newImage->SetOrigin(origin[0], origin[1], origin[2]);
    }

    double range[2] = { 0.0, 0.0 };
    double imageSpacing[3] = { 1.0, 1.0, 1.0 };
    newImage->GetScalarRange(range);
    newImage->GetSpacing(imageSpacing);
    int dims[3] = { 0, 0, 0 };
    newImage->GetDimensions(dims);

    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_vtkImage = newImage;
        m_scalarRange = { range[0], range[1] };
        m_imageSpacing = { imageSpacing[0], imageSpacing[1], imageSpacing[2] };
    }

    std::cout << "[Success] Loaded Volume: " << dims[0] << "x" << dims[1] << "x" << dims[2] << std::endl;
    SetLoadState(LoadState::Succeeded);
    SetLoadedFilePath(inputPath);
    return true;
}

bool BaseDataManager::SetTransformedDataSaved(const std::string& filePath, const std::array<double, 16>& transformMatrix)
{
    vtkSmartPointer<vtkImageData> imageCopy = vtkSmartPointer<vtkImageData>::New();
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        if (!m_vtkImage) return false;
        // 浅拷贝数据指针，避免在主线程引发锁竞争
        imageCopy->ShallowCopy(m_vtkImage);
    }

    //  VTK 逆变换矩阵
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(transformMatrix.data());
    matrix->Invert();

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    // 利用 vtkImageReslice 进行核心插值运算
    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(imageCopy);
    reslice->SetResliceTransform(transform);
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
    std::filesystem::path finalPath = pathObj.parent_path() / (pathObj.stem().string() + "_" + dimStr + pathObj.extension().string());
    
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
    std::cout << "[Export] Successfully saved transformed RAW to: " << filePath << "\n"
        << "[Export] IMPORTANT: New Dimensions are "
        << newDims[0] << " x " << newDims[1] << " x " << newDims[2] << std::endl;

    return true;
}