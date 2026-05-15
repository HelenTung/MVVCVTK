#pragma once

#include "AppTypes.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

class AppStateCommands {
public:
    static bool SetScalar(double& current,
        const double next,
        const double epsilon = 1e-6) {
        if (std::abs(current - next) <= epsilon) {
            return false;
        }

        current = next;
        return true;
    }

    template <typename T>
    static bool SetValue(T& current, const T& next) {
        if (current == next) {
            return false;
        }

        current = next;
        return true;
    }

    template <size_t N>
    static bool SetArray(std::array<double, N>& current,
        const std::array<double, N>& next,
        const double epsilon = 1e-6) {
        for (size_t index = 0; index < N; ++index) {
            if (std::abs(current[index] - next[index]) > epsilon) {
                current = next;
                return true;
            }
        }

        return false;
    }

    static bool SetTFNodes(std::vector<TFNode>& current,
        const std::vector<TFNode>& next) {
        if (current.size() != next.size()) {
            current = next;
            return true;
        }

        const bool same = std::equal(current.begin(), current.end(), next.begin(),
            [](const TFNode& left, const TFNode& right) {
                return std::abs(left.position - right.position) <= 1e-6 &&
                    std::abs(left.opacity - right.opacity) <= 1e-6 &&
                    std::abs(left.r - right.r) <= 1e-6 &&
                    std::abs(left.g - right.g) <= 1e-6 &&
                    std::abs(left.b - right.b) <= 1e-6;
            });
        if (same) {
            return false;
        }

        current = next;
        return true;
    }

    static bool SetMaterial(MaterialParams& current, const MaterialParams& next) {
        if (std::abs(current.ambient - next.ambient) <= 1e-6 &&
            std::abs(current.diffuse - next.diffuse) <= 1e-6 &&
            std::abs(current.specular - next.specular) <= 1e-6 &&
            std::abs(current.specularPower - next.specularPower) <= 1e-6 &&
            std::abs(current.opacity - next.opacity) <= 1e-6 &&
            current.shadeOn == next.shadeOn) {
            return false;
        }

        current = next;
        return true;
    }

    static bool SetBackground(BackgroundColor& current, const BackgroundColor& next) {
        if (std::abs(current.r - next.r) <= 1e-6 &&
            std::abs(current.g - next.g) <= 1e-6 &&
            std::abs(current.b - next.b) <= 1e-6) {
            return false;
        }

        current = next;
        return true;
    }

    static bool SetWindowLevel(WindowLevelParams& current, const WindowLevelParams& next) {
        if (std::abs(current.windowWidth - next.windowWidth) <= 1e-6 &&
            std::abs(current.windowCenter - next.windowCenter) <= 1e-6) {
            return false;
        }

        current = next;
        return true;
    }

    static bool SetVisibilityMask(uint32_t& current, uint32_t flagBit, bool show) {
        const uint32_t next = show ? (current | flagBit) : (current & ~flagBit);
        if (next == current) {
            return false;
        }

        current = next;
        return true;
    }

    static void SetFlagsMerged(UpdateFlags& current, UpdateFlags next) {
        current |= next;
    }
};