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
    // 解析文件名中的尺寸 (例如 data_512x512x200.raw)
    std::filesystem::path pathObj(filePath);
    std::string name = pathObj.filename().string();
    std::regex pattern(R"((\d+)[xX](\d+)[xX](\d+))");
    std::smatch matches;

    if (std::regex_search(name, matches, pattern) && matches.size() > 3) {
        m_dims[0] = std::stoi(matches[1].str());
        m_dims[1] = std::stoi(matches[2].str());
        m_dims[2] = std::stoi(matches[3].str());
    }
    else {
        return false; // 文件名格式不对
    }

    // 读取二进制数据
    size_t totalVoxels = m_dims[0] * m_dims[1] * m_dims[2];

    // 构建 VTK ImageData (Zero-Copy)
    m_vtkImage->SetDimensions(m_dims[0], m_dims[1], m_dims[2]);
    m_vtkImage->SetSpacing(m_spacing, m_spacing, m_spacing);
    m_vtkImage->SetOrigin(0, 0, 0);
    m_vtkImage->AllocateScalars(VTK_FLOAT, 1); // 申请内存，类型为float，连续一维

    // 文件流操作
    float* vtkDataPtr = static_cast<float*>(m_vtkImage->GetScalarPointer());
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;
    file.read(reinterpret_cast<char*>(vtkDataPtr), totalVoxels * sizeof(float));
    file.close();

    m_vtkImage->Modified();

    return true;
}

vtkSmartPointer<vtkImageData> RawVolumeDataManager::GetVtkImage() const { return m_vtkImage; }
