#pragma once
#include "MeasurementTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <string>

class MeasurementComputeService {
public:
    static double GetLength(const std::array<double, 3>& p0,
        const std::array<double, 3>& p1)
    {
        const double dx = p1[0] - p0[0];
        const double dy = p1[1] - p0[1];
        const double dz = p1[2] - p0[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    static double GetAngle(const std::array<double, 3>& p0,
        const std::array<double, 3>& vertex,
        const std::array<double, 3>& p2)
    {
        double v0[3] = {
            p0[0] - vertex[0],
            p0[1] - vertex[1],
            p0[2] - vertex[2]
        };
        double v1[3] = {
            p2[0] - vertex[0],
            p2[1] - vertex[1],
            p2[2] - vertex[2]
        };

        const double len0 = std::sqrt(v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2]);
        const double len1 = std::sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
        if (len0 <= 1e-6 || len1 <= 1e-6) {
            return 0.0;
        }

        const double dot = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
        const double cosValue = std::clamp(dot / (len0 * len1), -1.0, 1.0);
        return std::acos(cosValue) * 180.0 / std::acos(-1);
    }

    static std::array<double, 3> GetMidPoint(const std::array<double, 3>& p0,
        const std::array<double, 3>& p1)
    {
        return {
            (p0[0] + p1[0]) * 0.5,
            (p0[1] + p1[1]) * 0.5,
            (p0[2] + p1[2]) * 0.5
        };
    }

    static std::array<double, 3> GetWorldPoint(const std::array<double, 16>& modelMatrix,
        const std::array<double, 3>& modelPos)
    {
        const double x = modelPos[0];
        const double y = modelPos[1];
        const double z = modelPos[2];
        const double w = modelMatrix[3] * x
            + modelMatrix[7] * y
            + modelMatrix[11] * z
            + modelMatrix[15]; // 当前模型点经过 4x4 模型矩阵后的齐次坐标 w

        const double safeW = std::abs(w) > 1e-6 ? w : 1.0;
        return {
            (modelMatrix[0] * x + modelMatrix[4] * y + modelMatrix[8]  * z + modelMatrix[12]) / safeW,
            (modelMatrix[1] * x + modelMatrix[5] * y + modelMatrix[9]  * z + modelMatrix[13]) / safeW,
            (modelMatrix[2] * x + modelMatrix[6] * y + modelMatrix[10] * z + modelMatrix[14]) / safeW
        };
    }

    static std::array<double, 3> GetAngleTextPosition(const std::array<double, 3>& p0,
        const std::array<double, 3>& vertex,
        const std::array<double, 3>& p2)
    {
        double dir0[3] = {
            p0[0] - vertex[0],
            p0[1] - vertex[1],
            p0[2] - vertex[2]
        };
        double dir1[3] = {
            p2[0] - vertex[0],
            p2[1] - vertex[1],
            p2[2] - vertex[2]
        };

        const double len0 = std::sqrt(dir0[0] * dir0[0] + dir0[1] * dir0[1] + dir0[2] * dir0[2]);
        const double len1 = std::sqrt(dir1[0] * dir1[0] + dir1[1] * dir1[1] + dir1[2] * dir1[2]);
        if (len0 > 1e-6) {
            dir0[0] /= len0;
            dir0[1] /= len0;
            dir0[2] /= len0;
        }
        if (len1 > 1e-6) {
            dir1[0] /= len1;
            dir1[1] /= len1;
            dir1[2] /= len1;
        }

        std::array<double, 3> dir = {
            dir0[0] + dir1[0],
            dir0[1] + dir1[1],
            dir0[2] + dir1[2]
        };
        const double dirLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dirLen <= 1e-6) {
            dir = { dir0[0], dir0[1], dir0[2] };
        }   
        else {
            dir[0] /= dirLen;
            dir[1] /= dirLen;
            dir[2] /= dirLen;
        }

        const double radius = std::max(MeasurementComputeService::GetLength(vertex, p0),
            MeasurementComputeService::GetLength(vertex, p2)) * 0.25;
        return {
            vertex[0] + dir[0] * radius,    
            vertex[1] + dir[1] * radius,
            vertex[2] + dir[2] * radius
        };
    }
    
    static std::string GetValueText(double value, const std::string& unit)
    {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(2);
        oss << value;
        if (!unit.empty()) {
            oss << ' ' << unit;
        }
        return oss.str();
    }

    static std::string GetTypeText(MeasurementType type)
    {
        if (type == MeasurementType::Length) return "Length";
        if (type == MeasurementType::Angle) return "Angle";
        return "None";
    }

    static std::string GetStatusText(MeasurementStatus status)
    {
        if (status == MeasurementStatus::InProgress) return "InProgress";
        if (status == MeasurementStatus::Succeeded) return "Succeeded";
        if (status == MeasurementStatus::Invalid) return "Invalid";
        return "Idle";
    }
};