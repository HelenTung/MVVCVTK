#include "AlignmentSceneDisplay.h"
#include "AlignmentMath.h"
#include <vtkActor.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <map>
AlignmentSceneOverlay::AlignmentSceneOverlay(
    std::shared_ptr<const AlignmentSceneProduct> product)
    : m_product(std::move(product)) {
  std::map<vtkPolyData *, vtkSmartPointer<vtkPolyDataMapper>> mappers;
  for (const auto &surface : m_product->surfaces) {
    auto &mapper = mappers[surface.mesh->mesh];
    if (!mapper) {
      mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
      mapper->SetInputData(surface.mesh->mesh);
      mapper->ScalarVisibilityOff();
      mapper->StaticOn();
    }
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->PickableOn();
    actor->GetProperty()->SetColor(surface.color[0], surface.color[1],
                                   surface.color[2]);
    actor->GetProperty()->SetOpacity(surface.opacity);
    actor->GetProperty()->SetLighting(false);
    if (surface.wireframe)
      actor->GetProperty()->SetRepresentationToWireframe();
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(surface.localToModel.data());
    actor->SetUserMatrix(matrix);
    AttachProp(actor);
    m_actors.push_back(actor);
  }
}
void AlignmentSceneOverlay::SetOverlayState(const FeatureOverlayState &state) {
  for (std::size_t i = 0; i < m_actors.size(); ++i) {
    const auto matrix = AlignmentMath::Multiply(
        state.modelToWorld, m_product->surfaces[i].localToModel);
    // 更新现有矩阵，不创建变换后的网格或改动顶点、拓扑与标量。
    m_actors[i]->GetUserMatrix()->DeepCopy(matrix.data());
  }
}
