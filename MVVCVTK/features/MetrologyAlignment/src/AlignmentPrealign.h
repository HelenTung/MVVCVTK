#pragma once
#include "AlignmentGeometry.h"

using AlignmentDescriptor = std::array<double, 33>;
struct AlignmentFeatureCloud final {
    std::vector<AlignmentPoint> points;
    std::vector<AlignmentPoint> normals;
    std::vector<AlignmentDescriptor> descriptors;
    std::vector<std::uint8_t> valid;
};
struct AlignmentPrealignResult final {
    std::vector<AlignmentMatrix> poses;
    AlignmentPrealignStats statistics;
};
class AlignmentPrealign final {
  public:
    static bool GetParamsValid(const AlignmentPrealignParams &params);
    static AlignmentPrealignResult BuildCandidates(const AlignmentWork &work);
    // 私有算法边界，输入法向为符号提示；false 用于已经建立可靠法向的点集。
    static void BuildFeatures(AlignmentFeatureCloud &cloud, const AlignmentWork &work,
                              bool estimateNormals = true);
};
