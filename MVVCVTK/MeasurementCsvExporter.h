#pragma once
#include "MeasurementComputeService.h"
#include <fstream>
#include <string>
#include <vector>

class MeasurementCsvExporter {
public:
    static bool SetResultsFileSaved(const std::string& path,
        const std::vector<MeasurementResult>& results)
    {
        const std::string filePath = path.empty() ? "measurement_results.csv" : path;
        std::ofstream out(filePath, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }

        out << "Id,Type,Status,Value,Unit,Visible,IsHistorical"
            << ",P0X,P0Y,P0Z,P1X,P1Y,P1Z,P2X,P2Y,P2Z\n";

        for (const auto& result : results) {
            out << result.id << ','
                << MeasurementComputeService::GetTypeText(result.type) << ','
                << MeasurementComputeService::GetStatusText(result.status) << ','
                << result.value << ','
                << result.unit << ','
                << (result.visible ? 1 : 0) << ','
                << (result.isHistorical ? 1 : 0);

            for (size_t i = 0; i < 3; ++i) {
                if (i < result.worldPoints.size()) {
                    out << ',' << result.worldPoints[i][0]
                        << ',' << result.worldPoints[i][1]
                        << ',' << result.worldPoints[i][2];
                }
                else {
                    out << ",,,";
                }
            }
            out << '\n';
        }

        return true;
    }
};
