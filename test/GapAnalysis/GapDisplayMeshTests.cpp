// 回归：大量独立小孔隙按合并后内存计量，保留每个面的精确区域编号。
#include "Render/GapDisplayData.h"
#include "Data/DataGraphStore.h"
#include "Geometry/RoiEvaluator.h"
#include <vtkUnsignedCharArray.h>
#include <vtkTriangleFilter.h>
#include <iostream>

namespace {
bool Geometry() {
    std::atomic<bool> stopping{false};vtkNew<vtkImageData> image;
    image->SetExtent(-64,67,-1,4,10,15);image->SetSpacing(.7,1.2,2.3);image->SetOrigin(1e8,-3,9);
    const double direction[]{0,-1,0,1,0,0,0,0,1};image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_INT,1);std::fill_n(static_cast<int*>(image->GetScalarPointer()),image->GetNumberOfPoints(),0);
    VoidRegion region;region.id=2;region.voxelCount=130;region.volumeMM3=130;region.bbox={-63,66,1,1,12,12};
    for(int x=-63;x<=66;++x)*static_cast<int*>(image->GetScalarPointer(x,1,12))=region.id;
    const auto mesh=GapSurfaceBuilder::Build(image,{region},stopping);
    if(!mesh||mesh->GetNumberOfPoints()!=4*region.voxelCount+2||mesh->GetNumberOfCells()!=8*region.voxelCount)return false;
    const auto empty=GapSurfaceBuilder::Build(image,{},stopping);
    vtkNew<vtkTriangleFilter> triangulated;triangulated->SetInputData(empty);triangulated->Update();
    auto* emptyIds=triangulated->GetOutput()->GetCellData()->GetArray("gap.region.id");
    if(!emptyIds||emptyIds->GetNumberOfTuples()!=0||empty->GetCellData()->GetScalars()!=nullptr)return false;
    const auto* bounds=mesh->GetBounds();
    const double roundoff=8*std::numeric_limits<double>::epsilon()*1e8;
    if(std::abs(bounds[0]-(1e8-1.2*1.5))>roundoff||std::abs(bounds[1]-(1e8-1.2*.5))>roundoff
        ||std::abs(bounds[2]-(-3+.7*-63.5))>roundoff||std::abs(bounds[3]-(-3+.7*66.5))>roundoff
        ||std::abs(bounds[4]-(9+2.3*11.5))>roundoff||std::abs(bounds[5]-(9+2.3*12.5))>roundoff)return false;
    std::map<std::pair<vtkIdType,vtkIdType>,int> edges;vtkIdType size=0;const vtkIdType* corners=nullptr;mesh->GetPolys()->InitTraversal();
    while(mesh->GetPolys()->GetNextCell(size,corners))for(int j=0;j<3;++j)++edges[std::minmax(corners[j],corners[(j+1)%3])];
    if(!std::all_of(edges.begin(),edges.end(),[](const auto& edge){return edge.second==2;}))return false;
    // 仅重编号，跨块几何必须逐点不变；大物理原点和旋转方向不参与顶点焊接键。
    region.id=33554433;for(int x=-63;x<=66;++x)*static_cast<int*>(image->GetScalarPointer(x,1,12))=region.id;image->Modified();
    const auto renumbered=GapSurfaceBuilder::Build(image,{region},stopping);
    if(!renumbered||renumbered->GetNumberOfPoints()!=mesh->GetNumberOfPoints())return false;
    for(vtkIdType i=0;i<mesh->GetNumberOfPoints();++i){double p[3],q[3];mesh->GetPoint(i,p);renumbered->GetPoint(i,q);if(!std::equal(p,p+3,q))return false;}
    DataGraphStore store;GridGeometry3D grid;grid.extent={-64,67,-1,4,10,15};grid.dimensions={132,6,6};grid.spacing={.7,1.2,2.3};grid.origin={1e8,-3,9};std::copy(direction,direction+9,grid.direction.begin());
    const DataRevisionRef source{store.CreateDataEntityId(),1};DataTransaction tx;
    tx.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,std::make_shared<const std::vector<std::uint8_t>>(132*6*6,0))}};
    if(store.SetDataCommit(tx).status!=DataCommitStatus::Succeeded)return false;
    RoiPrimitive box;box.localToSource={1e9,0,0,0,0,1e9,0,0,0,0,1e9,0,0,0,0,1};
    const auto roi=RoiEvaluator::GetPreview(store.GetDataGraph(),{source,{{RoiNodeKind::Primitive,box}}});
    if(roi.error!=RoiError::None||!roi.roi)return false;
    const auto clipped=GapSurfaceBuilder::Build(image,{region},stopping,roi.roi);
    if(!clipped||clipped->GetNumberOfCells()!=mesh->GetNumberOfCells()||clipped->GetNumberOfPoints()!=mesh->GetNumberOfPoints())return false;
    RoiPrimitive half;half.shape=RoiShape::HalfSpace;half.origin={0,-2.741,0};half.normal={0,1,0};
    const auto selection=RoiEvaluator::GetPreview(store.GetDataGraph(),{source,{{RoiNodeKind::Primitive,half}}});
    if(!selection.roi)return false;
    const auto cut=GapSurfaceBuilder::Build(image,{region},stopping,selection.roi);
    if(!cut||!cut->GetNumberOfCells()||cut->GetNumberOfCells()>=mesh->GetNumberOfCells())return false;
    edges.clear();cut->GetPolys()->InitTraversal();
    while(cut->GetPolys()->GetNextCell(size,corners))for(int j=0;j<3;++j)++edges[std::minmax(corners[j],corners[(j+1)%3])];
    for(const auto& edge:edges) {
        if(edge.second==2)continue;if(edge.second!=1)return false;
        double a[3],b[3];cut->GetPoint(edge.first.first,a);cut->GetPoint(edge.first.second,b);
        if(std::abs(a[1]-half.origin[1])>1e-11||std::abs(b[1]-half.origin[1])>1e-11)return false;
    }
    const auto display=GapDisplayData::Build(image,{region},stopping,{},nullptr,nullptr,renumbered);
    if(!display||display->mesh->GetPoints()!=renumbered->GetPoints()||!renumbered->GetCellData()->GetArray("gap.region.id")
        ||!display->mesh->GetCellData()->GetArray("gap.display.id"))return false;
    // 三条相连细线的 bbox 临时字节数超过旧 64 MiB 限制；结果仍只含这些细线。
    vtkNew<vtkImageData> large;large->SetDimensions(408,408,408);large->AllocateScalars(VTK_INT,1);
    std::fill_n(static_cast<int*>(large->GetScalarPointer()),large->GetNumberOfPoints(),0);
    VoidRegion sparse;sparse.id=17;sparse.voxelCount=3*406-2;sparse.volumeMM3=static_cast<float>(sparse.voxelCount);sparse.bbox={1,406,1,406,1,406};
    for(int i=1;i<=406;++i){*static_cast<int*>(large->GetScalarPointer(i,1,1))=17;*static_cast<int*>(large->GetScalarPointer(406,i,1))=17;*static_cast<int*>(large->GetScalarPointer(406,406,i))=17;}
    const auto surface=GapSurfaceBuilder::Build(large,{sparse},stopping);
    if(!surface||!surface->GetNumberOfCells())return false;
    vtkNew<vtkImageData> binary;binary->SetDimensions(408,408,408);binary->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    std::fill_n(static_cast<unsigned char*>(binary->GetScalarPointer()),binary->GetNumberOfPoints(),0);
    for(int i=1;i<=406;++i){*static_cast<unsigned char*>(binary->GetScalarPointer(i,1,1))=1;*static_cast<unsigned char*>(binary->GetScalarPointer(406,i,1))=1;*static_cast<unsigned char*>(binary->GetScalarPointer(406,406,i))=1;}
    vtkNew<vtkFlyingEdges3D> reference;reference->SetInputData(binary);reference->SetValue(0,.5);reference->Update();
    if(surface->GetNumberOfCells()!=reference->GetOutput()->GetNumberOfCells()||surface->GetNumberOfPoints()!=reference->GetOutput()->GetNumberOfPoints())return false;
    std::cout<<"Gap geometry: rodPoints="<<mesh->GetNumberOfPoints()<<" sparseBBoxBytes="<<408ULL*408*408<<" sparseTriangles="<<surface->GetNumberOfCells()<<'\n';
    return true;
}
}
bool OccupiedBlocksMatch()
{
    vtkNew<vtkImageData> image;image->SetExtent(-65,64,-65,64,-65,64);image->AllocateScalars(VTK_INT,1);
    image->SetSpacing(.7,1.2,2.3);image->SetOrigin(1e8,-3,9);
    std::fill_n(static_cast<int*>(image->GetScalarPointer()),image->GetNumberOfPoints(),0);
    DataArray<VoidRegion> regions(3);
    for(std::size_t n=0;n<regions.size();++n){regions[n].id=static_cast<int>(33554433+2*n);regions[n].voxelCount=128;regions[n].bbox={-64,63,-64,63,-64,63};}
    for(int i=-64;i<=63;++i) {
        *static_cast<int*>(image->GetScalarPointer(i,i,i))=regions[0].id;
        *static_cast<int*>(image->GetScalarPointer(i,i,-1-i))=regions[1].id;
        *static_cast<int*>(image->GetScalarPointer(i,-1-i,i))=regions[2].id;
    }
    std::atomic<bool> stopping{false};GapRegionIndex parents;
    const auto directory=GetGapOccupiedBlocks(image,regions,parents,false,stopping);
    if(!directory||!directory->isIndexed||directory->blocks.size()>=3*27)return false;
    DataArray<GapSurfaceRange> indexedRanges,directRanges;
    const auto indexed=GapSurfaceBuilder::Build(image,regions,stopping,{},nullptr,&indexedRanges);
    const auto direct=GapSurfaceBuilder::Build(image,regions,stopping,{},nullptr,&directRanges,false);
    if(!indexed||!direct||indexed->GetNumberOfPoints()!=direct->GetNumberOfPoints()
        ||indexed->GetNumberOfCells()!=direct->GetNumberOfCells()||indexedRanges.size()!=directRanges.size())return false;
    for(vtkIdType i=0;i<indexed->GetNumberOfPoints();++i){double a[3],b[3];indexed->GetPoint(i,a);direct->GetPoint(i,b);if(!std::equal(a,a+3,b))return false;}
    auto* a=indexed->GetPolys()->GetConnectivityArray();auto* b=direct->GetPolys()->GetConnectivityArray();
    for(vtkIdType i=0;i<a->GetNumberOfValues();++i)if(a->GetComponent(i,0)!=b->GetComponent(i,0))return false;
    for(std::size_t i=0;i<indexedRanges.size();++i){const auto& x=indexedRanges[i];const auto& y=directRanges[i];if(x.id!=y.id||x.firstPoint!=y.firstPoint||x.pointCount!=y.pointCount||x.firstTriangle!=y.firstTriangle||x.triangleCount!=y.triangleCount)return false;}
    auto children=regions;for(auto& child:children){child.voxelCount=1;child.bbox={0,0,0,0,0,0};}
    const auto parentIndexed=GapSurfaceBuilder::Build(image,children,stopping,{},&regions);
    if(!parentIndexed||parentIndexed->GetNumberOfPoints()!=direct->GetNumberOfPoints()
        ||parentIndexed->GetNumberOfCells()!=direct->GetNumberOfCells())return false;
    for(vtkIdType i=0;i<direct->GetNumberOfPoints();++i){double p[3],q[3];parentIndexed->GetPoint(i,p);direct->GetPoint(i,q);if(!std::equal(p,p+3,q))return false;}
    std::cout<<"Gap occupied blocks="<<directory->blocks.size()<<" bboxBlocks="<<3*27<<'\n';return true;
}
int main()
{
    if(!Geometry()||!OccupiedBlocksMatch()){std::cerr<<"Gap binary geometry, block seam or ROI point reuse regression.\n";return 1;}
    vtkNew<GapLabelLookupTable> fast;
    const auto directory=std::make_shared<const GapRegionVolumes>(GapRegionVolumes{{60001,1},{33554433,2}});
    AnalysisColorStyle::RampParams ramp;ramp.blend=AnalysisColorStyle::BlendMode::Inclined;
    fast->SetRegions(directory,{1,2},ramp);
    vtkNew<vtkLookupTable> reference;
    reference->SetNumberOfTableValues(3);reference->IndexedLookupOn();
    reference->SetTableValue(0,0,0,0,0);reference->SetNanColor(.5,.5,.5,1);
    for(int i=0;i<2;++i){const auto color=AnalysisColorStyle::GetMappedColor({1,2},ramp,1+i);reference->SetTableValue(i+1,color.data());}
    vtkNew<vtkIntArray> smallIds; vtkNew<vtkStringArray> names;
    for (const int id : {0, 60001, 33554433}) {
        smallIds->InsertNextValue(id); names->InsertNextValue(std::to_string(id));
    }
    reference->SetAnnotations(smallIds,names);reference->Build();
    if(fast->GetNumberOfAnnotatedValues()!=0||fast->GetNumberOfTableValues()!=1)return 1;
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
    vtkNew<GapLabelLookupTable> isolated;isolated->DeepCopy(fast);isolated->SetOnlyRegion(33554433);
    std::array<unsigned char,4> selectedColor;std::copy_n(fast->MapValue(33554433),4,selectedColor.begin());
    if(isolated->GetOpacity(60001)!=0||isolated->GetOpacity(0)!=0||fast->GetOpacity(60001)==0
        ||!std::equal(selectedColor.begin(),selectedColor.end(),isolated->MapValue(33554433)))return 1;

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
        && display->labels->GetNumberOfTableValues() == 1
        && display->labels->GetNumberOfAnnotatedValues() == 0
        && ids && ids->GetNumberOfTuples() == 8LL * count;
    for (int index = 0; valid && index < count; ++index) {
        for (int cell = 0; cell < 8; ++cell)
            valid &= ids->GetValue(8LL * index + cell) == regions[index].id;
        valid &= *static_cast<int*>(image->GetScalarPointer(2 * index + 1, 1, 1)) == regions[index].id;
    }
    const auto recolored=GapDisplayData::CreateColors(*display,GapDisplayParams{});
    valid &= recolored&&recolored->regionVolumes==display->regionVolumes;
    vtkSmartPointer<vtkUnsignedCharArray> colors;
    colors.TakeReference(display->labels->MapScalars(ids, VTK_COLOR_MODE_MAP_SCALARS, 0));
    valid &= colors && colors->GetNumberOfComponents() == 4
        && colors->GetNumberOfTuples() == mesh->GetNumberOfCells();
    for (int index = 0; valid && index < count; ++index) {
        const auto* expected = display->volumes->MapValue(regions[index].volumeMM3);
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
