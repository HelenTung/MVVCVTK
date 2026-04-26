#pragma once
#include "AppTypes.h"
#include <vtkMatrix4x4.h>
#include <vtkTransform.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <array>
#include <cmath>

struct SliceExportData {
    Orientation orientation = Orientation::Top_down;
    std::array<double, 16> matrix = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    }; // 当前切片导出任务应使用的姿态矩阵
};

class InteractionComputeService {
public:
    static vtkSmartPointer<vtkMatrix4x4> GetModelMatrix(
        vtkMatrix4x4* currentMatrix,
        const double translate[3],
        const double rotate[3],
        const double scale[3])
    {
        auto transform = vtkSmartPointer<vtkTransform>::New();
        transform->PostMultiply();

        if (currentMatrix) {
            transform->SetMatrix(currentMatrix);
        }

        const double sx = (scale[0] == 0.0) ? 1.0 : scale[0];
        const double sy = (scale[1] == 0.0) ? 1.0 : scale[1];
        const double sz = (scale[2] == 0.0) ? 1.0 : scale[2];
        transform->Scale(sx, sy, sz);
        transform->RotateX(rotate[0]);
        transform->RotateY(rotate[1]);
        transform->RotateZ(rotate[2]);
        transform->Translate(translate[0], translate[1], translate[2]);

        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        matrix->DeepCopy(transform->GetMatrix());
        return matrix;
    }

    static void GetModelPositionFromWorld(
        vtkMatrix4x4* inverseMatrix,
        const double worldPos[3],
        double modelPos[3])
    {
        if (!inverseMatrix) return;

        double inPos[4] = { worldPos[0], worldPos[1], worldPos[2], 1.0 };
        double outPos[4] = { 0, 0, 0, 1 };
        inverseMatrix->MultiplyPoint(inPos, outPos);
        if (outPos[3] != 0.0) {
            modelPos[0] = outPos[0] / outPos[3];
            modelPos[1] = outPos[1] / outPos[3];
            modelPos[2] = outPos[2] / outPos[3];
        }
    }

    static void GetWorldPositionFromModel(
        vtkMatrix4x4* modelMatrix,
        const double modelPos[3],
        double worldPos[3])
    {
        if (!modelMatrix) return;

        double inPos[4] = { modelPos[0], modelPos[1], modelPos[2], 1.0 };
        double outPos[4] = { 0, 0, 0, 1 };
        modelMatrix->MultiplyPoint(inPos, outPos);
        if (outPos[3] != 0.0) {
            worldPos[0] = outPos[0] / outPos[3];
            worldPos[1] = outPos[1] / outPos[3];
            worldPos[2] = outPos[2] / outPos[3];
        }
    }

    static int GetSliceAxis(VizMode mode)
    {
        if (mode == VizMode::SliceFront_back) return 1;
        if (mode == VizMode::SliceLeft_right) return 0;
        return 2;
    }

    static void GetScrolledModelPosition(
        const double currentModel[3],
        int axis,
        int delta,
        const double spacing[3],
        const double bounds[6],
        double nextModel[3])
    {
        nextModel[0] = currentModel[0];
        nextModel[1] = currentModel[1];
        nextModel[2] = currentModel[2];

        if (axis >= 0 && axis < 3) {
            nextModel[axis] += static_cast<double>(delta) * spacing[axis]; // 当前滚轮步进对应的模型坐标增量
        }

        nextModel[0] = std::max(bounds[0], std::min(nextModel[0], bounds[1]));
        nextModel[1] = std::max(bounds[2], std::min(nextModel[1], bounds[3]));
        nextModel[2] = std::max(bounds[4], std::min(nextModel[2], bounds[5]));
    }

    static SliceExportData GetSliceExportData(
        const std::array<double, 16>& currentMatrix,
        VizMode mode,
        const std::array<double, 3>& cursorWorld,
        double angle)
    {
        SliceExportData exportData;
        auto baseMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        auto transform = vtkSmartPointer<vtkTransform>::New();
        baseMatrix->DeepCopy(currentMatrix.data());
        transform->PostMultiply();
        transform->SetMatrix(baseMatrix);

        transform->Translate(-cursorWorld[0], -cursorWorld[1], -cursorWorld[2]); // 当前切片旋转中心，对应联动光标世界坐标
        if (mode == VizMode::SliceFront_back) {
            exportData.orientation = Orientation::Front_back;
            transform->RotateY(angle);
        }
        else if (mode == VizMode::SliceLeft_right) {
            exportData.orientation = Orientation::Left_right;
            transform->RotateX(angle);
        }
        else if (mode == VizMode::SliceTop_down) {
            exportData.orientation = Orientation::Top_down;
            transform->RotateZ(angle);
        }
        transform->Translate(cursorWorld[0], cursorWorld[1], cursorWorld[2]);

        const double* matrixData = transform->GetMatrix()->GetData(); // 切片导出最终使用的 4x4 姿态矩阵数据
        std::copy(matrixData, matrixData + 16, exportData.matrix.begin());
        return exportData;
    }

    static WindowLevelParams GetWindowLevel(
        int totalDx,
        int totalDy,
        int viewWidth,
        int viewHeight,
        double startWW,
        double startWC)
    {
        WindowLevelParams windowLevel = { startWW, startWC }; // 当前交互结束后应写回状态的窗宽窗位结果
        if (viewWidth == 0 || viewHeight == 0) {
            return windowLevel;
        }

        double deltaWW = 3.0 * totalDx / static_cast<double>(viewWidth);   // 水平拖拽换算出的窗宽增量
        double deltaWC = 3.0 * totalDy / static_cast<double>(viewHeight);  // 垂直拖拽换算出的窗位增量

        if (std::abs(startWW) > 0.01) {
            deltaWW *= startWW;
        }
        else {
            deltaWW *= (startWW < 0.0 ? -0.01 : 0.01);
        }

        if (std::abs(startWC) > 0.01) {
            deltaWC *= startWC;
        }
        else {
            deltaWC *= (startWC < 0.0 ? -0.01 : 0.01);
        }

        if (startWW < 0.0) {
            deltaWW = -deltaWW;
        }
        if (startWC < 0.0) {
            deltaWC = -deltaWC;
        }

        windowLevel.windowWidth = std::max(0.01, startWW + deltaWW);
        windowLevel.windowCenter = startWC - deltaWC;
        return windowLevel;
    }
};
