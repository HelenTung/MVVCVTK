#include "DataManager.h"
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <vtkTIFFReader.h>
#include <algorithm>
#include <iostream>
#include <vtkStringArray.h> 
#include "MemMappedFile.h"

RawVolumeDataManager::RawVolumeDataManager() {
    m_vtkImage = vtkSmartPointer<vtkImageData>::New();
}

bool RawVolumeDataManager::LoadData(const std::string& filePath) {
    m_isLoading = true;
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
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_vtkImage) {
            m_vtkImage = nullptr;     // 置空防止悬垂指针
        }
    }

    // 创建全新的 vtkImageData 对象 
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(newDims[0], newDims[1], newDims[2]);
    newImage->SetSpacing(m_spacing, m_spacing, m_spacing);
    newImage->SetOrigin(0, 0, 0);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    // 读取数据到新内存
    size_t totalVoxels = static_cast<size_t>(newDims[0]) * static_cast<size_t>(newDims[1]) * static_cast<size_t>(newDims[2]);
    // float* vtkDataPtr = static_cast<float*>(newImage->GetScalarPointer());
    size_t expectedBytes = totalVoxels * sizeof(float);
    float* dst = static_cast<float*>(newImage->GetScalarPointer());

    // 使用 MemMappedFile 替代 std::ifstream读取
    MemMappedFile mmf;
    if (mmf.open(filePath)) {
        //   - 文件够大 → 只取前 expectedBytes
        //   - 文件偏小 → 只拷文件实际字节，剩余保持 AllocateScalars 的零值
        size_t copyBytes = (mmf.size() < expectedBytes) ? mmf.size() : expectedBytes;

        if (mmf.size() < expectedBytes) {
            std::cerr << "[Warn] File size (" << mmf.size()
                << ") < expected (" << expectedBytes
                << "). Partial load, remainder zeroed." << std::endl;
        }

        std::memcpy(dst, mmf.data(), copyBytes);
        // mmf 析构时自动 close()
    }
    else {
        // mmap 打开失败，fallback 到原始 ifstream
        std::cerr << "[Warn] MemMappedFile open failed, fallback to ifstream: "
            << filePath << std::endl;
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            m_isLoading = false;
            return false;
        }
        file.read(reinterpret_cast<char*>(dst),
            static_cast<std::streamsize>(expectedBytes));
        file.close();
    }


    newImage->Modified();
 
    // --- 提交  ---
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vtkImage = std::move(newImage); // 指针交换，旧数据若被渲染线程持有则暂存，否则释放
        m_dims[0] = newDims[0];
        m_dims[1] = newDims[1];
        m_dims[2] = newDims[2];
    }
    m_isLoading = false;
    return true;
}

bool RawVolumeDataManager::SetFromBuffer(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin)
{
    auto setState = [this](LoadState state, bool isLoading) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_loadState = state;
        }
        m_isLoading.store(isLoading);
        };

    if (!data || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        setState(LoadState::Failed, false);
        return false;
    }

    setState(LoadState::Loading, true);

    // ── 在调用方线程完成唯一一次分配 + 拷贝（只此一次，不再重复）────
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
    std::memcpy(dst, data, total * sizeof(float));  // 唯一一次拷贝

    // Modified() 暂不调用——留到主线程 ConsumeReconImage() 中调用，
    // 确保 VTK pipeline 脏标记传播在正确线程触发。

    {
        std::lock_guard<std::mutex> lock(m_reconMutex);
        m_pendingImage = std::move(newImage);   // 指针赋值，无拷贝
    }
    m_hasPendingImage.store(true);

    // LoadState::Succeeded 延迟到主线程 ConsumeReconImage() 设置，
    // 防止调用方在 vtkImage 还未提交时就查询到 Succeeded 状态。
    return true;
}

bool RawVolumeDataManager::ConsumeReconImage()
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
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vtkImage = incoming;
        m_dims[0] = incoming->GetDimensions()[0];
        m_dims[1] = incoming->GetDimensions()[1];
        m_dims[2] = incoming->GetDimensions()[2];
        m_spacing = incoming->GetSpacing()[0];
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_loadState = LoadState::Succeeded;
    }
    m_isLoading.store(false);

    return true;
}

vtkSmartPointer<vtkImageData> RawVolumeDataManager::GetVtkImage() const { 
       std::lock_guard<std::mutex> lock(m_mutex);
    return m_vtkImage; // 返回副本，线程安全
}

TiffVolumeDataManager::TiffVolumeDataManager() {
    // 初始化一个空的 ImageData 防止空指针访问
    m_vtkImage = vtkSmartPointer<vtkImageData>::New();
}

bool TiffVolumeDataManager::LoadData(const std::string& inputPath) {
	m_isLoading = true;
    // 路径检查
    std::filesystem::path pathObj(inputPath);
    if (!std::filesystem::exists(pathObj)) {
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
        std::cerr << "[Error] Exception during TIFF reading." << std::endl;
        return false;
    }

    auto output = reader->GetOutput();
    if (!output || output->GetDimensions()[0] == 0) return false;

    // --- 数据提交 (Back Buffer 策略) ---
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->ShallowCopy(output);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vtkImage = newImage;
    }

    int dims[3];
    m_vtkImage->GetDimensions(dims);
    std::cout << "[Success] Loaded Volume: " << dims[0] << "x" << dims[1] << "x" << dims[2] << std::endl;
	m_isLoading = false;
    return true;
}

vtkSmartPointer<vtkImageData> TiffVolumeDataManager::GetVtkImage() const {
    // 线程安全获取
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vtkImage;
}