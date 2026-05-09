#pragma once
#include "MeasurementTypes.h"

class IMeasurementStrategy {
public:
    virtual ~IMeasurementStrategy() = default;

    virtual MeasurementType GetMeasurementType() const = 0;
    virtual void SetSessionStateSynced(const MeasurementSessionState& state) = 0;
    virtual MeasurementStatus SetPointAdded(const double worldPos[3],
        const double modelPos[3]) = 0;
    virtual void SetPreviewPointUpdated(const double worldPos[3],
        const double modelPos[3]) = 0;
    virtual void SetPreviewCleared() = 0;
    virtual void SetLatest(bool isLatest) = 0;
    virtual void SetVisible(bool show) = 0;
    virtual bool GetVisible() const = 0;
    virtual bool GetFinished() const = 0;
    virtual uint64_t GetId() const = 0;
    virtual const MeasurementResult& GetResult() const = 0;
};
