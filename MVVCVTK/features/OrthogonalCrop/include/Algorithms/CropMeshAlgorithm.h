#pragma once
#include "Algorithms/CropAlgorithm.h"

class CropMeshAlgorithm final {
public:
    static CropMaterializationCandidate GetResult(vtkPolyData* mesh,
        const CropBuildParams& params,const std::vector<CropGeometry>& geometry,
        const std::function<bool()>& getStopRequested);
};
