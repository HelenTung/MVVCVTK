#include "DataManager.h"
#include <vtkFloatArray.h>
#include <vtkPointData.h>
#include <fstream>
#include <filesystem>
#include <regex>

RawVolumeDataManager::RawVolumeDataManager() {
    m_vtkImage = vtkSmartPointer<vtkImageData>::New();
}

bool RawVolumeDataManager::LoadData(const std::string& filePath) {
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

    // 创建全新的 vtkImageData 对象 (Back Buffer)
    auto newImage = vtkSmartPointer<vtkImageData>::New();
    newImage->SetDimensions(newDims[0], newDims[1], newDims[2]);
    newImage->SetSpacing(m_spacing, m_spacing, m_spacing);
    newImage->SetOrigin(0, 0, 0);
    newImage->AllocateScalars(VTK_FLOAT, 1);

    // 读取数据到新内存
    size_t totalVoxels = (size_t)newDims[0] * newDims[1] * newDims[2];
    float* vtkDataPtr = static_cast<float*>(newImage->GetScalarPointer());

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;
    file.read(reinterpret_cast<char*>(vtkDataPtr), totalVoxels * sizeof(float));
    file.close();

    newImage->Modified();

    // --- 提交  ---
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_vtkImage = newImage; // 指针交换，旧数据若被渲染线程持有则暂存，否则释放
        m_dims[0] = newDims[0];
        m_dims[1] = newDims[1];
        m_dims[2] = newDims[2];
    }

    return true;
}

vtkSmartPointer<vtkImageData> RawVolumeDataManager::GetVtkImage() const { 
       std::lock_guard<std::mutex> lock(m_mutex);
    return m_vtkImage; // 返回副本，线程安全
}
