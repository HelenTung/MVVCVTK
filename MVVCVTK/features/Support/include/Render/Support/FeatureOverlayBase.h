#pragma once

#include "Render/Contracts/FeatureOverlay.h"
#include "Render/Support/RenderTextStyle.h"

#include <vtkMatrix4x4.h>
#include <vtkProp.h>
#include <vtkProp3D.h>
#include <vtkWeakPointer.h>

#include <array>
#include <utility>
#include <vector>

// Feature 侧可复用的 overlay 骨架，只管理 prop、renderer 与模型变换。
class FeatureOverlayBase : public FeatureOverlay {
protected:
    // 文字与几何由同一 overlay 独占，挂接失败或退出时一起清理。
    void SetCaption(const char* text, int row)
    {
        if (!m_caption) {
            m_caption = vtkSmartPointer<vtkTextActor>::New();
            AttachProp(m_caption);
        }
        RenderTextStyle::SetCaption(*m_caption, text, row);
    }

    void SetCaptionVisible(bool isVisible)
    {
        if (m_caption) m_caption->SetVisibility(isVisible);
    }

    void AttachProp(vtkSmartPointer<vtkProp> prop)
    {
        if (prop) m_managedProps.push_back(std::move(prop));
    }

    void Set3DPropsTransform(
        const std::array<double, 16>& modelToWorld)
    {
        for (const auto& prop : m_managedProps) {
            auto* prop3D = vtkProp3D::SafeDownCast(prop);
            if (!prop3D) continue;
            auto* matrix = prop3D->GetUserMatrix();
            if (matrix) {
                matrix->DeepCopy(modelToWorld.data());
                continue;
            }
            auto nextMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            nextMatrix->DeepCopy(modelToWorld.data());
            prop3D->SetUserMatrix(nextMatrix);
        }
    }

public:
    ~FeatureOverlayBase() override = default;

    void AttachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) return;
        if (m_renderer
            && m_renderer.GetPointer() != renderer.GetPointer()) {
            for (const auto& prop : m_managedProps) {
                m_renderer->RemoveViewProp(prop);
            }
        }
        for (const auto& prop : m_managedProps) {
            renderer->AddViewProp(prop);
        }
        m_renderer = renderer;
    }

    void DetachRenderer(
        vtkSmartPointer<vtkRenderer> renderer) override
    {
        if (!renderer) return;
        for (const auto& prop : m_managedProps) {
            renderer->RemoveViewProp(prop);
        }
        if (m_renderer.GetPointer() == renderer.GetPointer()) {
            m_renderer = nullptr;
        }
    }

private:
    vtkSmartPointer<vtkTextActor> m_caption;
    std::vector<vtkSmartPointer<vtkProp>> m_managedProps;
    vtkWeakPointer<vtkRenderer> m_renderer;
};
