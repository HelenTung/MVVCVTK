#pragma once
#include "ThicknessAlgorithm.h"
#include "Host/TrustedDataPort.h"

namespace ThicknessData
{
inline const DataTypeId resultType{"org.mvvcvtk.wall-thickness.result", 1};
inline const DataFacetId resultFacet{"wall-thickness-result"};
inline constexpr std::string_view bindingName = "wall-thickness.active";
struct Record final
{
    ThicknessArchive archive;
    ThicknessAlgorithm::Field field;
    ThicknessStatistics statistics;
    std::vector<ThicknessRegion> regions;
    std::vector<DataExpectation> expectations;
};
class ResultPayload final : public IDataPayload
{
  public:
    explicit ResultPayload(Record record)
        : m_record(std::make_shared<const Record>(std::move(record)))
    {
    }
    DataTypeId GetDataType() const override
    {
        return resultType;
    }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const ResultPayload>(*this);
    }
    const Record &GetRecord() const noexcept
    {
        return *m_record;
    }

  private:
    std::shared_ptr<const Record> m_record;
};
bool SetType(TrustedDataPort &data);
bool GetCurrent(const DataGraphSnapshot &graph, const std::vector<DataExpectation> &expectations);
ThicknessAlgorithm::Work BuildWork(const DataGraphSnapshot &graph, const ThicknessArchive &archive,
                                   std::vector<DataExpectation> &expectations);
DataBinding GetBinding(const DataGraphSnapshot &graph);
std::shared_ptr<const ResultPayload> GetResult(const DataGraphSnapshot &graph,
                                               const DataRevisionRef &ref);
std::string GetParameters(const ThicknessArchive &archive);
} // namespace ThicknessData
