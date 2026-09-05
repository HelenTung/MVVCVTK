#include "Algorithms/ModelRotationAlgorithm.h"
#include <algorithm>
#include <cmath>

std::optional<ModelRotationAlgorithm::Point>
ModelRotationAlgorithm::GetModelCenter(const ImageDescriptor& image)
{
    Point index{};
    for (int axis = 0; axis < 3; ++axis) {
        if (image.extent[2 * axis] > image.extent[2 * axis + 1]
            || !std::isfinite(image.spacing[axis]) || image.spacing[axis] <= 0) return {};
        // extent 含两端，先转 double 避免 int 求和溢出。
        index[axis] = (static_cast<double>(image.extent[2 * axis])
            + image.extent[2 * axis + 1]) * 0.5 * image.spacing[axis];
    }
    Point center = image.origin;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            center[row] += image.direction[3 * row + column] * index[column];
        if (!std::isfinite(center[row])) return {};
    }
    return center;
}

ModelRotationAlgorithm::Point ModelRotationAlgorithm::GetWorldPoint(
    const Matrix& matrix, const Point& point)
{
    Point result{};
    for (int row = 0; row < 3; ++row)
        result[row] = matrix[row * 4] * point[0]
            + matrix[row * 4 + 1] * point[1]
            + matrix[row * 4 + 2] * point[2] + matrix[row * 4 + 3];
    return result;
}

std::optional<ModelRotationAlgorithm::Quaternion>
ModelRotationAlgorithm::GetAxisRotation(const Point& axis, double angleDeg)
{
    const double length = std::hypot(axis[0], axis[1], axis[2]);
    if (!std::isfinite(length) || length <= 1e-12 || !std::isfinite(angleDeg)) return {};
    constexpr double radiansPerDegree = 0.01745329251994329577;
    const double halfAngle = std::remainder(angleDeg, 360.0) * radiansPerDegree * 0.5;
    const double factor = std::sin(halfAngle) / length;
    return Quaternion{ std::cos(halfAngle), axis[0] * factor,
        axis[1] * factor, axis[2] * factor };
}

std::optional<ModelRotationAlgorithm::Quaternion>
ModelRotationAlgorithm::GetTrackballRotation(
    double startX, double startY, double x, double y,
    double radius, const Point& right, const Point& up, const Point& toward)
{
    if (!std::isfinite(radius) || radius <= 0) return {};
    const auto project = [radius, &right, &up, &toward](double px, double py) {
        px /= radius;
        py /= radius;
        const double distance = std::hypot(px, py);
        const double z = distance < 1 ? std::sqrt(std::max(0.0, 1 - distance * distance)) : 0;
        if (distance > 1) { px /= distance; py /= distance; }
        return Point{ px * right[0] + py * up[0] + z * toward[0],
            px * right[1] + py * up[1] + z * toward[1],
            px * right[2] + py * up[2] + z * toward[2] };
    };
    const auto from = project(startX, startY);
    const auto to = project(x, y);
    const double dot = from[0]*to[0] + from[1]*to[1] + from[2]*to[2];
    Quaternion rotation{ 1 + std::clamp(dot, -1.0, 1.0),
        from[1]*to[2] - from[2]*to[1],
        from[2]*to[0] - from[0]*to[2],
        from[0]*to[1] - from[1]*to[0] };
    if (dot < 0 && std::hypot(rotation[1],rotation[2],rotation[3]) < 1e-12) {
        // 对径点采用固定正交轴，避免四元数全零。
        const Point reference = std::abs(from[0]) < 0.8 ? Point{1,0,0} : Point{0,1,0};
        rotation = { 0, from[1]*reference[2] - from[2]*reference[1],
            from[2]*reference[0] - from[0]*reference[2],
            from[0]*reference[1] - from[1]*reference[0] };
    }
    return rotation;
}

std::optional<ModelRotationAlgorithm::Matrix>
ModelRotationAlgorithm::GetRotatedMatrix(
    const Matrix& start, const Point& worldCenter, Quaternion rotation)
{
    if (!std::all_of(start.begin(),start.end(),[](double value){return std::isfinite(value);})
        || !std::all_of(worldCenter.begin(),worldCenter.end(),[](double value){return std::isfinite(value);})
        || start[12] != 0 || start[13] != 0 || start[14] != 0 || start[15] != 1) return {};
    const double determinant = start[0]*(start[5]*start[10]-start[6]*start[9])
        - start[1]*(start[4]*start[10]-start[6]*start[8])
        + start[2]*(start[4]*start[9]-start[5]*start[8]);
    if (!std::isfinite(determinant) || determinant == 0) return {};
    double length = 0;
    for (double value : rotation) length = std::hypot(length, value);
    if (!std::isfinite(length) || length <= 1e-12) return {};
    for (double& value : rotation) value /= length;
    const double w = rotation[0], x = rotation[1], y = rotation[2], z = rotation[3];
    const std::array<double, 9> r{
        1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w),
        2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w),
        2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y) };
    Matrix result = start;
    // T(p) R T(-p) M0：只左乘本次刚性旋转，保留起始平移/缩放。
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            result[row*4 + column] = 0;
            for (int k = 0; k < 3; ++k)
                result[row*4 + column] += r[row*3+k] *
                    (start[k*4+column] - (column == 3 ? worldCenter[k] : 0));
            if (column == 3) result[row*4+column] += worldCenter[row];
        }
    }
    if (!std::all_of(result.begin(), result.end(),
            [](double value) { return std::isfinite(value); })) return {};
    return result;
}
