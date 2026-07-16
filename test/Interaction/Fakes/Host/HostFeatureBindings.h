#pragma once

#include "Host/HostCommandTypes.h"

#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

class HostFeatureBindings {
public:
    bool SendCommand(HostFeatureCommand command)
    {
        m_lastCommand = command;
        return std::visit(
            [this](auto&& featureCommand) -> bool {
                using Command = std::decay_t<decltype(featureCommand)>;
                if constexpr (std::is_same_v<Command, HostCropCommand>) {
                    return SendCropCommand(featureCommand);
                }
                else if constexpr (std::is_same_v<Command, HostGapCommand>) {
                    return SendGapCommand(featureCommand);
                }
                else if constexpr (std::is_same_v<Command, HostExitCommand>) {
                    ++m_featureExitCount;
                    return ExitCrop() || ExitGapView();
                }
                else {
                    // 新 feature domain 由其专用测试 fake 覆盖；通用 host fake 保持可编译并显式拒绝。
                    return false;
                }
            },
            std::move(command));
    }

    bool GetCropActive() const
    {
        return m_isCropActive;
    }

    bool GetGapView() const
    {
        return m_isGapView;
    }

    void SetCropActive(bool isActive)
    {
        m_isCropActive = isActive;
    }

    bool SetGapView(bool isActive)
    {
        m_isGapView = isActive;
        return true;
    }

    bool SetCommandResult(bool isSuccess)
    {
        m_isCommandSuccess = isSuccess;
        return true;
    }

    const HostGapViewRequest& GetLastGapRequest() const
    {
        return m_lastGapRequest;
    }

    const HostCropViewRequest& GetLastCropRequest() const
    {
        return m_lastCropRequest;
    }

    HostCropPreviewMode GetLastPreviewMode() const
    {
        return m_lastPreviewMode;
    }

    const HostFeatureCommand& GetLastCommand() const
    {
        return m_lastCommand;
    }

    int GetCropSendCount() const
    {
        return m_cropSendCount;
    }

    int GetFeatureExitCount() const
    {
        return m_featureExitCount;
    }

    int GetCropStartCount() const { return m_cropStartCount; }
    int GetCropBoxCount() const { return m_cropBoxCount; }
    int GetCropPlaneCount() const { return m_cropPlaneCount; }
    int GetCropViewCount() const { return m_cropViewCount; }
    int GetCropExitCount() const { return m_cropExitCount; }
    int GetGapStartCount() const { return m_gapStartCount; }
    int GetGapViewCount() const { return m_gapViewCount; }
    int GetGapLayerCount() const { return m_gapLayerCount; }
    int GetGapExitCount() const { return m_gapExitCount; }
    int GetCropInputCount() const { return m_cropInputCount; }
    int GetCropClearCount() const { return m_cropClearCount; }

    void ClearCropInput() const
    {
        ++m_cropClearCount;
    }

    bool SendCropInput()
    {
        ++m_cropInputCount;
        return true;
    }

private:
#if defined(_MSC_VER)
    // 测试 fake 必须与 production 保持同样的 action 穷尽门禁。
#pragma warning(push)
#pragma warning(4 : 4062)
#pragma warning(error : 4062)
#endif

    bool SendCropCommand(const HostCropCommand& command)
    {
        switch (command.action) {
        case HostCropAction::Start:
            ++m_cropStartCount;
            m_lastCropRequest = command.request;
            return SetCropActive();
        case HostCropAction::Box:
            ++m_cropBoxCount;
            m_lastCropRequest = command.request;
            return SetCropActive();
        case HostCropAction::Plane:
            ++m_cropPlaneCount;
            m_lastCropRequest = command.request;
            return SetCropActive();
        case HostCropAction::Preview:
            ++m_cropViewCount;
            m_lastCropRequest = command.request;
            m_lastPreviewMode = command.previewMode;
            return SetCropActive();
        case HostCropAction::Submit:
            ++m_cropSendCount;
            m_lastCropRequest = command.request;
            return m_isCommandSuccess;
        case HostCropAction::Exit:
            return ExitCrop();
        }
        return false;
    }

    bool SendGapCommand(const HostGapCommand& command)
    {
        switch (command.action) {
        case HostGapAction::Start:
            ++m_gapStartCount;
            m_lastGapRequest = command.request;
            return SetGapActive();
        case HostGapAction::Switch:
            ++m_gapViewCount;
            m_lastGapRequest = command.request;
            return SetGapActive();
        case HostGapAction::Overlay:
            ++m_gapLayerCount;
            return m_isCommandSuccess && m_isGapView;
        case HostGapAction::Exit:
            return ExitGapView();
        }
        return false;
    }

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    bool SetCropActive()
    {
        if (m_isCommandSuccess) {
            m_isCropActive = true;
        }
        return m_isCommandSuccess;
    }

    bool SetGapActive()
    {
        if (m_isCommandSuccess) {
            m_isGapView = true;
        }
        return m_isCommandSuccess;
    }

    bool ExitCrop()
    {
        ++m_cropExitCount;
        if (!m_isCommandSuccess || !m_isCropActive) {
            return false;
        }
        m_isCropActive = false;
        return true;
    }

    bool ExitGapView()
    {
        ++m_gapExitCount;
        if (!m_isCommandSuccess || !m_isGapView) {
            return false;
        }
        m_isGapView = false;
        return true;
    }

    bool m_isCropActive = false;
    bool m_isGapView = false;
    bool m_isCommandSuccess = true;
    HostCropViewRequest m_lastCropRequest;
    HostCropPreviewMode m_lastPreviewMode = HostCropPreviewMode::KeepInside;
    HostGapViewRequest m_lastGapRequest;
    HostFeatureCommand m_lastCommand;
    int m_cropSendCount = 0;
    int m_featureExitCount = 0;
    int m_cropStartCount = 0;
    int m_cropBoxCount = 0;
    int m_cropPlaneCount = 0;
    int m_cropViewCount = 0;
    int m_cropExitCount = 0;
    int m_gapStartCount = 0;
    int m_gapViewCount = 0;
    int m_gapLayerCount = 0;
    int m_gapExitCount = 0;
    int m_cropInputCount = 0;
    mutable int m_cropClearCount = 0;
};
