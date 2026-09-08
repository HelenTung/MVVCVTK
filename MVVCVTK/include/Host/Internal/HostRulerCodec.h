#pragma once

#include "Host/Types/HostSessionTypes.h"
#include "Render/Internal/RulerMetrics.h"
#include <optional>

// Host 值类型与 Render 私有值隔离，不能让 Render 反向依赖 Host 头。
class HostRulerCodec final {
public:
    static std::optional<RulerParams> BuildParams(const HostRulerParams& source) {
        RulerParams result;
        switch (source.unit) {
        case HostRulerUnit::Auto: result.unit = RulerUnit::Auto; break;
        case HostRulerUnit::Millimeter: result.unit = RulerUnit::Millimeter; break;
        case HostRulerUnit::Micrometer: result.unit = RulerUnit::Micrometer; break;
        default: return std::nullopt;
        }
        switch (source.position) {
        case HostRulerPosition::BottomLeft: result.position = RulerPosition::BottomLeft; break;
        case HostRulerPosition::BottomRight: result.position = RulerPosition::BottomRight; break;
        case HostRulerPosition::TopLeft: result.position = RulerPosition::TopLeft; break;
        case HostRulerPosition::TopRight: result.position = RulerPosition::TopRight; break;
        default: return std::nullopt;
        }
        result.targetPixels = source.targetPixels;
        result.fontSize = source.fontSize;
        result.color = source.color;
        return RulerMetrics::GetParamsValid(result) ? std::optional<RulerParams>{ result } : std::nullopt;
    }
    static HostRulerParams GetParams(const RulerParams& source) {
        HostRulerParams result;
        switch (source.unit) {
        case RulerUnit::Auto: result.unit = HostRulerUnit::Auto; break;
        case RulerUnit::Millimeter: result.unit = HostRulerUnit::Millimeter; break;
        case RulerUnit::Micrometer: result.unit = HostRulerUnit::Micrometer; break;
        }
        switch (source.position) {
        case RulerPosition::BottomLeft: result.position = HostRulerPosition::BottomLeft; break;
        case RulerPosition::BottomRight: result.position = HostRulerPosition::BottomRight; break;
        case RulerPosition::TopLeft: result.position = HostRulerPosition::TopLeft; break;
        case RulerPosition::TopRight: result.position = HostRulerPosition::TopRight; break;
        }
        result.targetPixels = source.targetPixels;
        result.fontSize = source.fontSize;
        result.color = source.color;
        return result;
    }
    static HostRulerState GetState(const RulerState& source) {
        HostRulerState result;
        switch (source.status) {
        case RulerStatus::Pending: result.status = HostRulerStatus::Pending; break;
        case RulerStatus::Visible: result.status = HostRulerStatus::Visible; break;
        case RulerStatus::Hidden: result.status = HostRulerStatus::Hidden; break;
        case RulerStatus::NoData: result.status = HostRulerStatus::NoData; break;
        case RulerStatus::InvalidGeometry: result.status = HostRulerStatus::InvalidGeometry; break;
        case RulerStatus::InvalidTransform: result.status = HostRulerStatus::InvalidTransform; break;
        case RulerStatus::UnsupportedProjection: result.status = HostRulerStatus::UnsupportedProjection; break;
        case RulerStatus::ViewportTooSmall: result.status = HostRulerStatus::ViewportTooSmall; break;
        case RulerStatus::InvalidScale: result.status = HostRulerStatus::InvalidScale; break;
        }
        result.lengthMm = source.lengthMm;
        result.lengthPixels = source.lengthPixels;
        result.label = source.label;
        result.dataRevision = source.dataRevision;
        result.bindingRevision = source.bindingRevision;
        return result;
    }
};
