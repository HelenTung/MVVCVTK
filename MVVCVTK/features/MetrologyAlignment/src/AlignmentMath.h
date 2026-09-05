#pragma once

#include "Host/MetrologyAlignmentHostTypes.h"
#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>

namespace AlignmentMath {
using Vec = cv::Vec3d;
inline Vec Vector(const AlignmentPoint &p) {
    return {p[0], p[1], p[2]};
}
inline AlignmentPoint Point(const Vec &p) {
    return {p[0], p[1], p[2]};
}
inline bool Finite(const Vec &p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
inline Vec Unit(const Vec &p) {
    const auto n = cv::norm(p);
    return n > 1e-15 ? p / n : Vec{};
}
inline Vec Rotate(const AlignmentMatrix &m, const Vec &p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2], m[4] * p[0] + m[5] * p[1] + m[6] * p[2],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2]};
}
inline Vec Transform(const AlignmentMatrix &m, const Vec &p) {
    return Rotate(m, p) + Vec(m[3], m[7], m[11]);
}
inline AlignmentMatrix Inverse(const AlignmentMatrix &m) {
    AlignmentMatrix r{m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1};
    const auto t = -Rotate(r, {m[3], m[7], m[11]});
    r[3] = t[0];
    r[7] = t[1];
    r[11] = t[2];
    return r;
}
inline AlignmentMatrix Multiply(const AlignmentMatrix &a, const AlignmentMatrix &b) {
    AlignmentMatrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r[4 * i + j] += a[4 * i + k] * b[4 * k + j];
    return r;
}
inline bool Rigid(const AlignmentMatrix &m) {
    for (const auto v : m)
        if (!std::isfinite(v))
            return false;
    if (std::abs(m[12]) + std::abs(m[13]) + std::abs(m[14]) + std::abs(m[15] - 1) > 1e-9)
        return false;
    const Vec x(m[0], m[1], m[2]), y(m[4], m[5], m[6]), z(m[8], m[9], m[10]);
    return std::abs(x.dot(x) - 1) + std::abs(y.dot(y) - 1) + std::abs(z.dot(z) - 1) +
                   std::abs(x.dot(y)) + std::abs(x.dot(z)) + std::abs(y.dot(z)) <
               1e-8 &&
           std::abs(x.cross(y).dot(z) - 1) < 1e-8;
}
inline Vec Tangent(const Vec &n) {
    return Unit(n.cross(std::abs(n[0]) < 0.8 ? Vec(1, 0, 0) : Vec(0, 1, 0)));
}
inline AlignmentMatrix Increment(const cv::Mat &delta, double lengthScale, const Vec &pivot) {
    const Vec w(delta.at<double>(0) / lengthScale, delta.at<double>(1) / lengthScale,
                delta.at<double>(2) / lengthScale);
    const double angle = cv::norm(w);
    const Vec n = angle > 1e-15 ? w / angle : Vec(0, 0, 1);
    const double c = std::cos(angle), s = std::sin(angle), v = 1 - c;
    AlignmentMatrix r{c + n[0] * n[0] * v,
                      n[0] * n[1] * v - n[2] * s,
                      n[0] * n[2] * v + n[1] * s,
                      0,
                      n[1] * n[0] * v + n[2] * s,
                      c + n[1] * n[1] * v,
                      n[1] * n[2] * v - n[0] * s,
                      0,
                      n[2] * n[0] * v - n[1] * s,
                      n[2] * n[1] * v + n[0] * s,
                      c + n[2] * n[2] * v,
                      0,
                      0,
                      0,
                      0,
                      1};
    const auto t = pivot - Rotate(r, pivot) +
                   Vec(delta.at<double>(3), delta.at<double>(4), delta.at<double>(5));
    r[3] = t[0];
    r[7] = t[1];
    r[11] = t[2];
    return r;
}
struct Rank final {
    int rank = 0;
    cv::Mat inverse;
    cv::Mat nullspace;
    std::vector<double> values;
    double condition = 1;
};
inline Rank Decompose(const cv::Mat &a, double tolerance) {
    Rank result;
    if (a.rows == 0) {
        result.inverse = cv::Mat::zeros(a.cols, 0, CV_64F);
        result.nullspace = cv::Mat::eye(a.cols, a.cols, CV_64F);
        return result;
    }
    // 高矩阵只需薄 U，避免 N 个测量点产生 N×N 工作数组。
    cv::SVD svd(a, a.rows < a.cols ? cv::SVD::FULL_UV : 0);
    const double maximum = svd.w.empty() ? 0 : svd.w.at<double>(0);
    result.inverse = cv::Mat::zeros(a.cols, a.rows, CV_64F);
    for (int i = 0; i < svd.w.rows; ++i) {
        const double value = svd.w.at<double>(i);
        result.values.push_back(value);
        if (value > tolerance * std::max(1.0, maximum)) {
            result.inverse += svd.vt.row(i).t() * svd.u.col(i).t() / value;
            ++result.rank;
            result.condition = maximum / value;
        }
    }
    result.nullspace = result.rank == a.cols ? cv::Mat(a.cols, 0, CV_64F)
                                             : cv::Mat(svd.vt.rowRange(result.rank, a.cols).t());
    return result;
}
} // namespace AlignmentMath
