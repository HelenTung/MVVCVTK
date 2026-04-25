#pragma once
#include "BaseVisualStrategy.h"
#include "IMeasurementStrategy.h"
#include <vtkActor.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkLineSource.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkSmartPointer.h>
#include <array>
#include <vector>

class LengthMeasurementStrategy : public BaseVisualStrategy, public IMeasurementStrategy {
public:
    explicit LengthMeasurementStrategy(uint64_t id);

    MeasurementType GetMeasurementType() const override { return MeasurementType::Length; }
    MeasurementStatus SetPointAdded(const double worldPos[3],
        const double modelPos[3]) override;
    void SetPreviewPointUpdated(const double worldPos[3],
        const double modelPos[3]) override;
    void SetPreviewCleared() override;
    void SetLatest(bool isLatest) override;
    void SetVisible(bool show) override;
    bool GetVisible() const override { return m_result.visible; }
    bool GetFinished() const override { return m_result.status == MeasurementStatus::Succeeded; }
    uint64_t GetId() const override { return m_result.id; }
    const MeasurementResult& GetResult() const override { return m_result; }

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override {}
    void SetVisualState(const RenderParams& params, UpdateFlags flags) override;

private:
    void SetPointSetUpdated();
    void SetPreviewVisualUpdated();
    void SetVisualUpdated();
    void SetStyleUpdated();
    void SetWorldPointsSynced(const std::array<double, 16>& modelMatrix);

    vtkSmartPointer<vtkLineSource> m_lineSource;             // 当前长度测量使用的线段数据源，预览阶段只更新末端点
    vtkSmartPointer<vtkPolyDataMapper> m_lineMapper;
    vtkSmartPointer<vtkActor> m_lineActor;
    vtkSmartPointer<vtkPoints> m_pointPositions;             // 当前已确认落点对应的静态点集，只有落点变化时才重建
    vtkSmartPointer<vtkPolyData> m_pointPolyData;
    vtkSmartPointer<vtkPolyDataMapper> m_pointMapper;
    vtkSmartPointer<vtkActor> m_pointActor;
    vtkSmartPointer<vtkBillboardTextActor3D> m_textActor;    // 当前长度值对应的 3D 文字标注
    MeasurementResult m_result;                              // 当前长度测量对外暴露的统一结果快照
    std::array<double, 3> m_previewWorldPoint = { 0.0, 0.0, 0.0 }; // 当前长度测量尚未确认的预览世界端点
    std::array<double, 3> m_previewModelPoint = { 0.0, 0.0, 0.0 }; // 当前长度测量预览端点对应的模型坐标
    bool m_hasPreviewPoint = false;
    std::vector<std::array<double, 3>> m_worldPoints; // 当前长度测量已确认的世界坐标点序列
    std::vector<std::array<double, 3>> m_modelPoints; // 当前长度测量已确认的模型坐标点序列
};
