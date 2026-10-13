// 测试宿主的公共区域选择：固定精确修订，不改变当前输入。
#pragma once
#include "Host/VtkAppHostSession.h"
#include "UiText.h"
#include <QJsonValue>
namespace Manual {
ParameterChoices GetRoiChoices(VtkAppHostSession& session, DataRevisionRef source, bool includeArchived = false);
std::optional<DataRevisionRef> GetSelectedRoi(VtkAppHostSession& session,
    const QJsonValue& value, DataRevisionRef source);
}
