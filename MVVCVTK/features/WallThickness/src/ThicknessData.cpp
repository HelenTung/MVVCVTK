#include "ThicknessData.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ThicknessData
{
bool SetType(TrustedDataPort &data)
{
    if (data.SetDataType({resultType,
                          {resultFacet},
                          [](const IDataPayload &payload, std::string &message)
                          {
                              const auto *p = dynamic_cast<const ResultPayload *>(&payload);
                              if (!p)
                              {
                                  message = "Invalid thickness result payload.";
                                  return false;
                              }
                              const auto &r = p->GetRecord();
                              const bool valid =
                                  r.field.samples && r.field.neighbors &&
                                  r.field.samples->size() == r.field.neighbors->size() &&
                                  r.field.subdivisions > 0 && r.field.subdivisions <= 64 &&
                                  ThicknessAlgorithm::GetParamsValid(r.archive.params) &&
                                  ThicknessAlgorithm::GetEvaluationValid(r.archive.evaluation) &&
                                  ThicknessAlgorithm::GetConfigValid(r.archive.limits);
                              if (!valid)
                                  message = "Invalid thickness result schema.";
                              return valid;
                          }}))
        return true;
    const auto graph = data.GetDataGraph();
    return graph.view &&
           graph.view->GetDataFacets(resultType) == std::vector<DataFacetId>{resultFacet};
}
bool GetCurrent(const DataGraphSnapshot &graph, const std::vector<DataExpectation> &expectations)
{
    if (!graph.view)
        return false;
    for (const auto &e : expectations)
    {
        if (e.kind == DataExpectationKind::Binding)
        {
            const auto b =
                graph.view->GetDataBinding(e.binding).value_or(DataBinding{e.binding, {}, 0});
            if (b.revision != e.expectedBindingRevision ||
                (e.isTargetChecked && b.target != e.expectedTarget))
                return false;
        }
        else
        {
            if (!graph.view->GetData({e.entityId, e.expectedGeneration}))
                return false;
            if (e.expectedGeneration < std::numeric_limits<DataGeneration>::max() &&
                graph.view->GetData({e.entityId, e.expectedGeneration + 1}))
                return false;
        }
    }
    return true;
}
ThicknessAlgorithm::Work BuildWork(const DataGraphSnapshot &graph, const ThicknessArchive &archive,
                                   std::vector<DataExpectation> &expectations)
{
    if (!graph.view)
        throw std::invalid_argument("No data graph.");
    const auto &input = archive.input;
    if (!GetDataRevisionRefValid(input.source) || !GetDataRevisionRefValid(input.labels) ||
        !GetDataRevisionRefValid(input.mesh) || input.materialLabel == 0 ||
        static_cast<unsigned>(input.unit) < 1 || static_cast<unsigned>(input.unit) > 2)
        throw std::invalid_argument(
            "Explicit input revisions, material and length unit are required.");
    const auto source = graph.view->GetData(input.source),
               labels = graph.view->GetData(input.labels), mesh = graph.view->GetData(input.mesh);
    ThicknessAlgorithm::Work work;
    work.archive = archive;
    work.source =
        source ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload) : nullptr;
    work.labels =
        labels ? std::dynamic_pointer_cast<const LabelMap3DPayload>(labels->payload) : nullptr;
    work.mesh = mesh ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(mesh->payload) : nullptr;
    if (!work.source || !work.labels || !work.mesh)
        throw std::invalid_argument("Input payload types differ.");
    std::set<DataRevisionRef> all;
    const auto collect = [&](const DataRevisionRef &root)
    {
        std::set<DataRevisionRef> visited;
        std::vector<DataRevisionRef> pending{root};
        while (!pending.empty())
        {
            const auto ref = pending.back();
            pending.pop_back();
            if (!visited.insert(ref).second)
                continue;
            if (visited.size() > 4096)
                throw std::invalid_argument("Input lineage exceeds bounded traversal.");
            const auto data = graph.view->GetData(ref);
            if (!data)
                throw std::invalid_argument("Input lineage revision missing.");
            for (const auto &upstream : data->inputs)
                pending.push_back(upstream.source);
        }
        all.insert(visited.begin(), visited.end());
        return visited.find(input.source) != visited.end();
    };
    if (!collect(input.labels) || !collect(input.mesh))
        throw std::invalid_argument("Labels and surface must derive from the exact source volume.");
    collect(input.source);
    expectations.clear();
    for (const auto &ref : all)
    {
        DataExpectation e;
        e.entityId = ref.entityId;
        e.expectedGeneration = ref.generation;
        expectations.push_back(std::move(e));
    }
    std::set<std::string> bindingNames;
    const auto addBinding = [&](const DataBinding &b)
    {
        if (!bindingNames.insert(b.name).second)
            return;
        DataExpectation e;
        e.kind = DataExpectationKind::Binding;
        e.binding = b.name;
        e.expectedBindingRevision = b.revision;
        e.isTargetChecked = true;
        e.expectedTarget = b.target;
        expectations.push_back(std::move(e));
    };
    // 捕获现有输入/上游绑定，换实体而不是同实体升代也能触发失效。
    for (const auto &b : graph.view->GetDataBindings())
        if (b.target && all.find(*b.target) != all.end())
            addBinding(b);
    const std::array<std::pair<std::string, DataRevisionRef>, 3> named{
        {{input.sourceBinding, input.source},
         {input.labelsBinding, input.labels},
         {input.meshBinding, input.mesh}}};
    for (const auto &namedBinding : named)
        if (!namedBinding.first.empty())
        {
            const auto b = graph.view->GetDataBinding(namedBinding.first);
            if (!b || b->target != namedBinding.second)
                throw std::invalid_argument("Explicit binding does not match input.");
            addBinding(*b);
        }
    if (!GetCurrent(graph, expectations))
        throw std::invalid_argument("Input lineage is stale.");
    work.cancelled = std::make_shared<std::atomic<bool>>(false);
    work.deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(archive.limits.deadlineMilliseconds);
    return work;
}
DataBinding GetBinding(const DataGraphSnapshot &graph)
{
    return graph.view ? graph.view->GetDataBinding(bindingName)
                            .value_or(DataBinding{std::string(bindingName), {}, 0})
                      : DataBinding{std::string(bindingName), {}, 0};
}
std::shared_ptr<const ResultPayload> GetResult(const DataGraphSnapshot &graph,
                                               const DataRevisionRef &ref)
{
    const auto data = graph.view ? graph.view->GetData(ref) : nullptr;
    return data && data->type == resultType
               ? std::dynamic_pointer_cast<const ResultPayload>(data->payload)
               : nullptr;
}
std::string GetParameters(const ThicknessArchive &a)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto ref = [&](const DataRevisionRef &value)
    {
        for (const auto byte : value.entityId.bytes)
            out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
        out << std::dec << ':' << value.generation << ';';
    };
    out << a.schemaVersion << ';' << std::quoted(a.algorithmVersion) << ';';
    ref(a.input.source);
    ref(a.input.labels);
    ref(a.input.mesh);
    out << std::quoted(a.input.sourceBinding) << ';' << std::quoted(a.input.labelsBinding) << ';'
        << std::quoted(a.input.meshBinding) << ';' << a.input.materialLabel << ';'
        << unsigned(a.input.unit) << ';';
    const auto &p = a.params;
    out << p.maxDistance << ';' << p.sampleSpacing << ';' << p.reverseTolerance << ';'
        << p.maxFitResidual << ';' << p.maxLocalizationSigma << ';' << p.minSupportRatio << ';'
        << p.coneAngleDegrees << ';' << p.directionCount << ';' << p.minOppositeCosine << ';'
        << p.sharpNormalCosine << ';' << p.ambiguityAbsolute << ';' << p.ambiguityRelative << ';'
        << p.maxBoundaryError << ';' << bool(p.evaluationBounds) << ';';
    if (p.evaluationBounds)
        for (double v : *p.evaluationBounds)
            out << v << ';';
    const auto &e = a.evaluation;
    out << e.lower << ';' << e.upper << ';' << e.histogramRange[0] << ';' << e.histogramRange[1]
        << ';' << e.histogramBins << ';' << e.minRegionArea << ';' << a.limits.maxWorkingBytes
        << ';' << a.limits.maxSamples << ';' << a.limits.deadlineMilliseconds << ';'
        << a.limits.stopTimeoutMilliseconds;
    return out.str();
}
} // namespace ThicknessData
