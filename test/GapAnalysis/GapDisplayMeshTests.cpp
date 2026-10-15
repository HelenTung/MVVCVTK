// 回归：大量独立小孔隙按合并后内存计量，保留每个面的精确区域编号。
#include "Render/GapDisplayData.h"
#include <iostream>

int main()
{
    constexpr int count = 70000;
    vtkNew<vtkImageData> image;
    image->SetDimensions(2 * count + 1, 3, 3);
    image->AllocateScalars(VTK_INT, 1);
    auto* values = static_cast<int*>(image->GetScalarPointer());
    std::fill_n(values, image->GetNumberOfPoints(), 0);
    std::vector<VoidRegion> regions;
    regions.reserve(count);
    for (int index = 0; index < count; ++index) {
        VoidRegion region;
        region.id = 33554433 + 2 * index;
        region.voxelCount = 1;
        region.volumeMM3 = 1;
        const int x = 2 * index + 1;
        region.bbox = {x, x, 1, 1, 1, 1};
        *static_cast<int*>(image->GetScalarPointer(x, 1, 1)) = region.id;
        regions.push_back(region);
    }
    std::atomic<bool> stopping{false};
    std::string reason;
    const auto display = GapDisplayData::Build(image, regions, stopping, {}, nullptr, &reason);
    if (!display || !display->mesh || !display->labels) {
        std::cerr << "Many-region display failed: " << reason << '\n';
        return 1;
    }
    auto* mesh = display->mesh.GetPointer();
    auto* ids = vtkIntArray::SafeDownCast(mesh->GetCellData()->GetScalars());
    // 独立单体素等值面为八面体：6 个顶点、8 个三角形。
    bool valid = mesh->GetNumberOfPoints() == 6LL * count
        && mesh->GetNumberOfCells() == 8LL * count
        && mesh->GetActualMemorySize() * 1024ULL < 256ULL * 1024 * 1024
        && display->labels->GetNumberOfTableValues() == count + 1
        && ids && ids->GetNumberOfTuples() == 8LL * count;
    for (int index = 0; valid && index < count; ++index) {
        for (int cell = 0; cell < 8; ++cell)
            valid &= ids->GetValue(8LL * index + cell) == regions[index].id;
        valid &= *static_cast<int*>(image->GetScalarPointer(2 * index + 1, 1, 1)) == regions[index].id;
    }
    for (const int index : {0, count / 2, count - 1})
        valid &= display->labels->GetAnnotatedValueIndex(vtkVariant(regions[index].id)) == index + 1;
    std::cout << "regions=" << count << " points=" << mesh->GetNumberOfPoints()
        << " triangles=" << mesh->GetNumberOfCells()
        << " mergedKiB=" << mesh->GetActualMemorySize() << '\n';
    stopping = true;
    valid &= !GapDisplayData::Build(image, regions, stopping);
    return valid ? 0 : 1;
}
