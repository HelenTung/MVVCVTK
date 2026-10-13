#include "RoiSelection.h"
#include "JsonInput.h"
namespace Manual {
ParameterChoices GetRoiChoices(VtkAppHostSession& session, DataRevisionRef source, bool includeArchived)
{
    ParameterChoices choices;
    for (const auto& roi : session.GetRoiDescriptors(includeArchived)) {
        if (roi.definition.source != source) continue;
        const auto ref = GetRefText(roi.revision);
        const auto name = QString::fromStdString(roi.metadata.name);
        const auto group = QString::fromStdString(roi.metadata.group);
        choices.append({ref, (group.isEmpty() ? QString() : group + " / ")
            + (name.isEmpty() ? "未命名区域" : name) + (roi.metadata.isArchived ? "（已归档）" : "") + " · " + ref.right(12)});
    }
    return choices;
}
std::optional<DataRevisionRef> GetSelectedRoi(VtkAppHostSession& session,
    const QJsonValue& value, DataRevisionRef source)
{
    if (value.isUndefined() || value.isNull() || (value.isString() && value.toString().isEmpty()))
        return {};
    const auto ref = GetRef(value);
    const auto roi = session.GetRoiDescriptor(ref);
    if (!roi) throw std::invalid_argument("所选区域修订不可用，请重新选择区域");
    if (roi->metadata.isArchived) throw std::invalid_argument("所选区域已归档，请恢复后再选用");
    if (roi->definition.source != source)
        throw std::invalid_argument("所选区域与本次业务的源数据修订不一致");
    return ref;
}
}
