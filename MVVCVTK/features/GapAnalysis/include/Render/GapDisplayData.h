#pragma once

#include "GapAnalysisTypes.h"
#include "Render/Support/AnalysisColorStyle.h"
#include <vtkAppendPolyData.h>
#include <vtkCellData.h>
#include <vtkFlyingEdges3D.h>
#include <vtkIntArray.h>
#include <vtkPointData.h>
#include <vtkVariant.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkNew.h>
#include <atomic>
#include <cmath>
#include <limits>
#include <set>

// 私有显示产物不发布到 DataGraph，正式标签、网格与供应商统计完全独立。
struct GapDisplayData final {
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkLookupTable> labels;
    vtkSmartPointer<vtkLookupTable> volumes;
    bool hasRegions = false;

    static std::shared_ptr<const GapDisplayData> Build(vtkImageData* image,
        const std::vector<VoidRegion>& regions, const std::atomic<bool>& isStopping)
    {
        if (!image || image->GetScalarType() != VTK_INT
            || image->GetNumberOfScalarComponents() != 1) return {};
        constexpr std::size_t roiByteLimit = 64U * 1024U * 1024U;
        constexpr std::size_t meshByteLimit = 256U * 1024U * 1024U;
        auto result = std::make_shared<GapDisplayData>();
        std::array<double, 2> range{std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest()};
        std::set<std::int32_t> ids;
        for (const auto& region : regions) {
            if (region.id <= 0 || !ids.insert(region.id).second
                || !std::isfinite(region.volumeMM3) || region.volumeMM3 < 0) return {};
            range[0] = std::min(range[0], double(region.volumeMM3));
            range[1] = std::max(range[1], double(region.volumeMM3));
        }
        result->hasRegions = !regions.empty();
        if (regions.empty()) range = {0, 1};
        // 单值结果只扩展颜色映射区间，不改变任何缺陷值。
        if (range[0] == range[1]) range[1] = std::nextafter(range[1],
            std::numeric_limits<double>::infinity());
        result->volumes = AnalysisColorStyle::BuildRamp(range, false);
        result->labels = vtkSmartPointer<vtkLookupTable>::New();
        result->labels->SetNumberOfTableValues(static_cast<vtkIdType>(regions.size() + 1));
        result->labels->IndexedLookupOn();
        result->labels->SetAnnotation(vtkVariant(0), "background");
        result->labels->SetTableValue(0, 0, 0, 0, 0);
        result->labels->SetNanColor(0.5, 0.5, 0.5, 1);
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        std::size_t totalMeshBytes = 0;
        int sourceExtent[6]; image->GetExtent(sourceExtent);
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (isStopping.load()) return {};
            const auto& region = regions[index];
            double rgb[3]; result->volumes->GetColor(region.volumeMM3, rgb);
            result->labels->SetAnnotation(vtkVariant(region.id), std::to_string(region.id));
            result->labels->SetTableValue(static_cast<vtkIdType>(index + 1), rgb[0], rgb[1], rgb[2], 1);
            if (region.voxelCount == 0) continue;
            // bbox 是供应商的含端点网格索引；外围背景只用于封闭显示表面。
            int extent[6]; std::size_t count = 1;
            for (int axis = 0; axis < 3; ++axis) {
                if (region.bbox[2 * axis] < sourceExtent[2 * axis]
                    || region.bbox[2 * axis + 1] > sourceExtent[2 * axis + 1]
                    || region.bbox[2 * axis] > region.bbox[2 * axis + 1]
                    || region.bbox[2 * axis] == std::numeric_limits<int>::min()
                    || region.bbox[2 * axis + 1] == std::numeric_limits<int>::max()) return {};
                extent[2 * axis] = region.bbox[2 * axis] - 1;
                extent[2 * axis + 1] = region.bbox[2 * axis + 1] + 1;
                const auto size = std::size_t(std::int64_t(extent[2 * axis + 1])
                    - extent[2 * axis] + 1);
                if (size > roiByteLimit / count) return {};
                count *= size;
            }
            auto mask = vtkSmartPointer<vtkImageData>::New();
            mask->CopyStructure(image); mask->SetExtent(extent);
            mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
            auto* values = static_cast<unsigned char*>(mask->GetScalarPointer());
            std::fill_n(values, count, static_cast<unsigned char>(0));
            for (int z = region.bbox[4]; z <= region.bbox[5]; ++z) {
                if (isStopping.load()) return {};
                for (int y = region.bbox[2]; y <= region.bbox[3]; ++y) {
                    auto* source = static_cast<const int*>(image->GetScalarPointer(region.bbox[0], y, z));
                    auto* target = static_cast<unsigned char*>(mask->GetScalarPointer(region.bbox[0], y, z));
                    for (int x = region.bbox[0]; x <= region.bbox[1]; ++x)
                        *target++ = *source++ == region.id ? 1 : 0;
                }
            }
            auto filter = vtkSmartPointer<vtkFlyingEdges3D>::New();
            vtkNew<vtkCallbackCommand> cancel;
            cancel->SetClientData(const_cast<std::atomic<bool>*>(&isStopping));
            cancel->SetCallback([](vtkObject* object,unsigned long,void* data,void*) {
                if(static_cast<const std::atomic<bool>*>(data)->load())
                    static_cast<vtkAlgorithm*>(object)->AbortExecuteOn();
            });
            filter->AddObserver(vtkCommand::ProgressEvent,cancel);
            filter->SetInputData(mask); filter->SetValue(0, 0.5);
            filter->ComputeNormalsOff(); filter->ComputeScalarsOff(); filter->Update();
            if (isStopping.load()) return {};
            auto poly = vtkSmartPointer<vtkPolyData>::New();
            poly->ShallowCopy(filter->GetOutput());
            auto labels = vtkSmartPointer<vtkIntArray>::New();
            labels->SetName("gap.display.id");
            labels->SetNumberOfTuples(poly->GetNumberOfCells());
            labels->FillComponent(0, region.id);
            poly->GetCellData()->SetScalars(labels);
            const auto bytes = std::size_t(poly->GetActualMemorySize()) * 1024;
            if (bytes > meshByteLimit - totalMeshBytes) return {};
            totalMeshBytes += bytes;
            append->AddInputData(poly);
        }
        result->labels->Build();
        result->mesh = vtkSmartPointer<vtkPolyData>::New();
        if (!regions.empty()) {
            if (isStopping.load()) return {};
            append->Update(); result->mesh->ShallowCopy(append->GetOutput());
        }
        return isStopping.load() ? nullptr : result;
    }
};
