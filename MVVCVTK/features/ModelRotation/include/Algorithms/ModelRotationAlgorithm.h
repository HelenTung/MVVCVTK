#pragma once
#include "Data/ImageReadTypes.h"
#include <array>
#include <optional>

class ModelRotationAlgorithm final {
public:
    using Matrix = std::array<double, 16>;
    using Point = std::array<double, 3>;
    using Quaternion = std::array<double, 4>; // w,x,y,z
    static std::optional<Point> GetModelCenter(const ImageDescriptor& image);
    static Point GetWorldPoint(const Matrix& matrix, const Point& point);
    static std::optional<Quaternion> GetAxisRotation(const Point& axis, double angleDeg);
    static std::optional<Quaternion> GetTrackballRotation(
        double startX, double startY, double x, double y,
        double radius, const Point& right, const Point& up, const Point& toward);
    static std::optional<Matrix> GetRotatedMatrix(
        const Matrix& start, const Point& worldCenter, Quaternion rotation);
};
