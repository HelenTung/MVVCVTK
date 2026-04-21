#pragma once
// =====================================================================
// VolumeTransformService.h
//
// 专注于模型坐标变换（Model Space <-> World Space），
//         与 VTK 渲染管线、策略缓存完全解耦。
// =====================================================================
#include "AppState.h"
#include <vtkMatrix4x4.h>
#include <vtkTransform.h>
#include <vtkSmartPointer.h>

class VolumeTransformService {
public:
    // ----------------------------------------------------------------
    // 构造：注入共享状态（依赖注入，不持有 DataManager / Renderer）
    // ----------------------------------------------------------------
    explicit VolumeTransformService(std::shared_ptr<SharedInteractionState> state)
        : m_sharedState(std::move(state))
    {
        // 初始化为单位矩阵，对应"未做任何变换"的初始状态
        m_cachedModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        m_cachedInverseModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
        m_cachedModelMatrix->Identity();
        m_cachedInverseModelMatrix->Identity();
    }

    // ----------------------------------------------------------------
    // GetModelPositionFromWorld：世界坐标 → 模型坐标
    // ----------------------------------------------------------------
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const
    {
        double inPos[4] = { worldPos[0], worldPos[1], worldPos[2], 1.0 };
        double outPos[4] = { 0, 0, 0, 1 };
        m_cachedInverseModelMatrix->MultiplyPoint(inPos, outPos);
        // 齐次除法：缩放变换后 w 可能不为 1
        if (outPos[3] != 0.0) {
            modelPos[0] = outPos[0] / outPos[3];
            modelPos[1] = outPos[1] / outPos[3];
            modelPos[2] = outPos[2] / outPos[3];
        }
    }

    // ----------------------------------------------------------------
    // GetWorldPositionFromModel：模型坐标 → 世界坐标
    // ----------------------------------------------------------------
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const
    {
        double inPos[4] = { modelPos[0], modelPos[1], modelPos[2], 1.0 };
        double outPos[4] = { 0, 0, 0, 1 };
        m_cachedModelMatrix->MultiplyPoint(inPos, outPos);
        if (outPos[3] != 0.0) {
            worldPos[0] = outPos[0] / outPos[3];
            worldPos[1] = outPos[1] / outPos[3];
            worldPos[2] = outPos[2] / outPos[3];
        }
    }

    // ----------------------------------------------------------------
    // SetModelMatrix：把 VTK Actor 的 UserMatrix 同步回 SharedState
    // ----------------------------------------------------------------
    void SetModelMatrix(vtkMatrix4x4* mat)
    {
        if (!mat) return;

        // 写入 SharedState（内部有 mutex，线程安全）
        std::array<double, 16> matData = {0};
        std::memcpy(matData.data(), mat->GetData(), 16 * sizeof(double));
        m_sharedState->SetModelMatrix(matData);

        // 更新本地缓存，供 GetModelPositionFromWorld / GetWorldPositionFromModel 高频调用
        m_cachedModelMatrix->DeepCopy(mat);
        m_cachedInverseModelMatrix->DeepCopy(mat);
        m_cachedInverseModelMatrix->Invert();
    }

    // ----------------------------------------------------------------
    // SetModelTransform：通过 平移/旋转/缩放 参数构建矩阵并写入 SharedState
    // ----------------------------------------------------------------
    void SetModelTransform(double translate[3], double rotate[3], double scale[3])
    {
        auto transform = vtkSmartPointer<vtkTransform>::New();
        // PostMultiply() 实际上执行的是左乘（把新的变换操作乘在总矩阵的左边），而它默认的 PreMultiply() 才是右乘（把新操作乘在右边）
        transform->PostMultiply();

		// 从当前模型矩阵开始累积变换，保持连续交互的平滑性（而不是每次都从单位矩阵开始）
        transform->SetMatrix(m_cachedModelMatrix);
		// Pw = T * Rz * Ry * Rx * S * C * Pm，其中 Pm 是模型坐标，Pw 是世界坐标,C是之前的变换，S/R/T 是本次交互的缩放/旋转/平移
        double sx = (scale[0] == 0.0) ? 1.0 : scale[0];
        double sy = (scale[1] == 0.0) ? 1.0 : scale[1];
        double sz = (scale[2] == 0.0) ? 1.0 : scale[2];
		transform->Scale(sx, sy, sz);
        transform->RotateX(rotate[0]);
        transform->RotateY(rotate[1]);
        transform->RotateZ(rotate[2]);
        transform->Translate(translate[0], translate[1], translate[2]);

        std::array<double, 16> matData = {0};
        std::memcpy(matData.data(),
            transform->GetMatrix()->GetData(),
            16 * sizeof(double));
        m_sharedState->SetModelMatrix(matData);
        // 同时刷新本地缓存
        m_cachedModelMatrix->DeepCopy(transform->GetMatrix());
        m_cachedInverseModelMatrix->DeepCopy(transform->GetMatrix());
        m_cachedInverseModelMatrix->Invert();
    }

    // ----------------------------------------------------------------
    // SetModelTransformReset：回到单位矩阵（不做任何变换）
    // ----------------------------------------------------------------
    void SetModelTransformReset()
    {
        std::array<double, 16> identity = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };
        m_sharedState->SetModelMatrix(identity);
        m_cachedModelMatrix->Identity();
        m_cachedInverseModelMatrix->Identity();
    }

    // ----------------------------------------------------------------
    // 提供给 MedicalVizService 的只读访问（避免再持有一份 sharedState 引用）
    // ----------------------------------------------------------------
    const vtkMatrix4x4* GetCachedModelMatrix()        const { return m_cachedModelMatrix; }
    const vtkMatrix4x4* GetCachedInverseModelMatrix() const { return m_cachedInverseModelMatrix; }

private:
    std::shared_ptr<SharedInteractionState> m_sharedState;
    vtkSmartPointer<vtkMatrix4x4>           m_cachedModelMatrix;
    vtkSmartPointer<vtkMatrix4x4>           m_cachedInverseModelMatrix;
};