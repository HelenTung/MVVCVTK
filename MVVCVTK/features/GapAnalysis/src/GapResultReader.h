#pragma once
#include "Host/GapHostTypes.h"
#include "Data/DataPayloads.h"

std::shared_ptr<const GapResultReader> BuildGapResultReader(DataRevisionRef result,DataRevisionRef source,
    std::shared_ptr<const RecordTablePayload> table,std::shared_ptr<const SurfaceMeshPayload> mesh,
    GridGeometry3D geometry);
