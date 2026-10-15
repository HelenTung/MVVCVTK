// 回归：大量独立小孔隙按合并后内存计量，保留每个面的精确区域编号。
#include "Render/GapDisplayData.h"
#include <vtkUnsignedCharArray.h>
#include <iostream>

int main()
{
    vtkNew<GapLabelLookupTable> fast;
    fast->SetNumberOfTableValues(3); fast->IndexedLookupOn();
    fast->SetTableValue(0, 0, 0, 0, 0);
    fast->SetTableValue(1, .2, .4, .8, .7);
    fast->SetTableValue(2, .8, .3, .1, .6);
    vtkNew<vtkIntArray> smallIds; vtkNew<vtkStringArray> names;
    for (const int id : {0, 60001, 33554433}) {
        smallIds->InsertNextValue(id); names->InsertNextValue(std::to_string(id));
    }
    fast->SetAnnotations(smallIds, names); fast->Build();
    vtkNew<vtkLookupTable> reference; reference->DeepCopy(fast);
    double samples[]{0, 60001, 33554433, -1, 1.5,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
    for (const double alpha : {0.0, .35, 1.0}) {
        fast->SetAlpha(alpha); reference->SetAlpha(alpha);
        for (const int format : {VTK_LUMINANCE, VTK_LUMINANCE_ALPHA, VTK_RGB, VTK_RGBA}) {
            unsigned char actual[28]{}, expected[28]{};
            fast->MapScalarsThroughTable2(samples, actual, VTK_DOUBLE, 7, 1, format);
            reference->MapScalarsThroughTable2(samples, expected, VTK_DOUBLE, 7, 1, format);
            if (!std::equal(actual, actual + 7 * format, expected)) {
                std::cerr << "Fast label colors differ from VTK categorical colors.\n"; return 1;
            }
        }
    }
    vtkNew<GapLabelLookupTable> copy; copy->DeepCopy(fast);
    if (!std::equal(copy->MapValue(33554433), copy->MapValue(33554433) + 4, reference->MapValue(33554433)))
        return 1;

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
        region.volumeMM3 = static_cast<float>(1 + index % 3);
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
    vtkSmartPointer<vtkUnsignedCharArray> colors;
    colors.TakeReference(display->labels->MapScalars(ids, VTK_COLOR_MODE_MAP_SCALARS, 0));
    valid &= colors && colors->GetNumberOfComponents() == 4
        && colors->GetNumberOfTuples() == mesh->GetNumberOfCells();
    for (int index = 0; valid && index < count; ++index) {
        const auto* expected = display->labels->GetPointer(index + 1);
        for (int cell = 0; cell < 8; ++cell) {
            unsigned char actual[4]; colors->GetTypedTuple(8LL * index + cell, actual);
            valid &= std::equal(actual, actual + 4, expected);
        }
    }
    std::cout << "regions=" << count << " points=" << mesh->GetNumberOfPoints()
        << " triangles=" << mesh->GetNumberOfCells()
        << " mergedKiB=" << mesh->GetActualMemorySize() << '\n';
    stopping = true;
    valid &= !GapDisplayData::Build(image, regions, stopping);
    return valid ? 0 : 1;
}
