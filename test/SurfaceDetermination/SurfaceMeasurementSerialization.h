#pragma once
#include "SurfaceDeterminationAlgorithm.h"
#include <ostream>

// 仓内审计序列化：性能与资源字段不参与数值一致性比较。
namespace SurfaceAudit {
template <class T> void Write(std::ostream &stream, const T &value) {
    stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
inline void WriteResult(std::ostream &stream, const SurfaceAlgorithmResult &result,bool full=false) {
    Write(stream, result.status);
    Write(stream, result.failureReason);
    Write(stream, result.parameterFingerprint);
    Write(stream, result.points.size());
    for (const auto &p : result.points) {
        for (auto x : p.seedPositionModel)
            Write(stream, x);
        for (auto x : p.seedNormalModel)
            Write(stream, x);
        for (auto x : p.positionModel)
            Write(stream, x);
        for (auto x : p.normalModel)
            Write(stream, x);
        Write(stream, p.localThreshold);
        Write(stream, p.gradientMagnitude);
        Write(stream, p.validSupportRatio);
        Write(stream, p.flags);
        Write(stream, p.objectIndex);
        Write(stream, p.interfaceIndex);
    }
    Write(stream, result.triangleIndices.size());
    for (auto x : result.triangleIndices)
        Write(stream, x);
    for (auto x : result.triangleValidity)
        Write(stream, x);
    Write(stream, result.objects.size());
    for (const auto &o : result.objects) {
        Write(stream, o.pointCount);
        Write(stream, o.triangleCount);
        Write(stream, o.areaValidity);
        Write(stream, o.volumeValidity);
        Write(stream, o.areaModelUnit2.has_value());
        Write(stream, o.areaModelUnit2.value_or(0));
        Write(stream, o.volumeModelUnit3.has_value());
        Write(stream, o.volumeModelUnit3.value_or(0));
    }
    if(!full)return;
    // 旧字节流作为前缀保留；v2补齐全部公开点/对象/接口计量字段，排除性能计时与资源统计。
    Write(stream,std::uint32_t{2});
    for(const auto byte:result.sourceRevision.entityId.bytes)Write(stream,byte);
    Write(stream,result.sourceRevision.generation);Write(stream,result.algorithmRevision);
    Write(stream,result.method);Write(stream,result.initialIsoValue);
    const auto recipe=SurfaceRecipeCodec::BuildText(result.resolvedParams);
    Write(stream,recipe.size());stream.write(recipe.data(),std::streamsize(recipe.size()));
    Write(stream,result.acceptedPointCount);Write(stream,result.lowContrastPointCount);
    Write(stream,result.rejectedPointCount);Write(stream,result.truncatedPointCount);Write(stream,result.nonManifoldObjectCount);
    Write(stream,result.isoEstimate.has_value());
    if(result.isoEstimate){const auto &iso=*result.isoEstimate;
        Write(stream,iso.isoValue);Write(stream,iso.backgroundValue);Write(stream,iso.materialValue);
        Write(stream,iso.sampleCount);Write(stream,iso.excludedSampleCount);Write(stream,iso.separationRatio);
    }
    for(const auto &p:result.points){
        Write(stream,p.contrast);Write(stream,p.fitResidual);Write(stream,p.offsetFromSeed);
        Write(stream,p.estimatedLocalizationSigma);Write(stream,p.crossingCount);Write(stream,p.overrideIndex);
        Write(stream,p.transitionWidthModel);Write(stream,p.pairedSeparationModel);
    }
    for(const auto &o:result.objects){
        Write(stream,o.objectIndex);Write(stream,o.interfaceIndex);Write(stream,o.firstPoint);Write(stream,o.firstTriangle);
        for(const auto value:o.boundsModel)Write(stream,value);
        Write(stream,o.validAreaRatio);Write(stream,o.isClosed);Write(stream,o.isManifold);
        Write(stream,o.isTruncated);Write(stream,o.isOrientationValid);
    }
    Write(stream,result.interfaces.size());
    for(const auto &entry:result.interfaces){
        Write(stream,entry.materials.materialA);Write(stream,entry.materials.materialB);
        Write(stream,entry.canonicalId.size());stream.write(entry.canonicalId.data(),std::streamsize(entry.canonicalId.size()));
        Write(stream,entry.firstPoint);Write(stream,entry.pointCount);Write(stream,entry.firstTriangle);Write(stream,entry.triangleCount);
    }
    for(const auto extent:result.execution.processedExtent)Write(stream,extent);
}
} // namespace SurfaceAudit
