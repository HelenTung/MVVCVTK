#include "Preview/CropPreviewPlug.h"
#include "Render/Strategies/CropOverlay.h"
#include "PlanarTestSuites.h"
#include "Algorithms/OrthogonalCropAlgorithm.h"

#include <vtkActor.h>
#include <vtkAutoInit.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp3D.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

class CropPreviewCases final {
public:

inline static constexpr double kPlaneTolerance = 1e-9;

class CropViewStub final : public InteractiveService {
public:
    CropViewStub()
        : m_actor(vtkSmartPointer<vtkActor>::New())
        , m_mapper(vtkSmartPointer<vtkPolyDataMapper>::New())
    {
        m_actor->SetMapper(m_mapper);
    }

    vtkPolyDataMapper* GetMapper() const
    {
        return m_mapper;
    }

    vtkProp3D* GetMainProp() override
    {
        return m_actor;
    }

    std::array<double, 16> GetModelMatrix() override
    {
        return CropGeometryAlgorithm::GetIdentityMatrix();
    }

    int GetNavigationAxis() const override
    {
        return -1;
    }

    void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy>) override
    {
    }

    void RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy>) override
    {
    }

    void ClearOverlayStrategies() override
    {
    }

    void SetRenderContext(
        vtkSmartPointer<vtkRenderWindow>,
        vtkSmartPointer<vtkRenderer>) override
    {
    }

    void SendUpdates() override
    {
    }

    bool GetDirty() const override
    {
        return false;
    }

    void SetDirty() override
    {
    }

    bool ResetDirty() override
    {
        return false;
    }

    void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy>) override
    {
    }

    void GetModelPositionFromWorld(
        const double worldPos[3],
        double modelPos[3]) const override
    {
        std::copy_n(worldPos, 3, modelPos);
    }

    void GetWorldPositionFromModel(
        const double modelPos[3],
        double worldPos[3]) const override
    {
        std::copy_n(modelPos, 3, worldPos);
    }

private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
};

bool SetExpect(bool isExpected, const char* message)
{
    if (!isExpected) {
        std::cerr << message << '\n';
    }
    return isExpected;
}

OrthogonalCropResult GetPreviewResult(CropShape geometryType)
{
    OrthogonalCropRequest request;
    request.dataSource = OrthogonalCropDataSource::PolyData;
    request.operation = OrthogonalCropOperation::Preview;
    request.geometryType = geometryType;
    request.removalMode = CropRemovalMode::KeepInside;

    CropDataModel cropData;
    cropData.boxToInputModelMatrix = CropGeometryAlgorithm::GetBoxMatrix(
        CropGeometryAlgorithm::GetCanonicalBounds());
    cropData.inputModelBounds = CropGeometryAlgorithm::GetCanonicalBounds();
    cropData.planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    cropData.planeNormalInInputModel = { 0.0, 0.0, 1.0 };

    OrthogonalCropResult result;
    result.resolvedDataSource = request.dataSource;
    result.resolvedOperation = request.operation;
    result.resolvedGeometryType = request.geometryType;
    result.resolvedRemovalMode = request.removalMode;
    result.cropDataModel = cropData;
    result.isSucceeded = true;
    return result;
}

bool StartBoxKeepCase()
{
    CropPreviewPlug previewPlug;
    auto target = std::make_shared<CropViewStub>();
    auto reference = std::make_shared<CropViewStub>();
    auto overlay = std::make_shared<CropOverlay>();
    auto result = GetPreviewResult(CropShape::Box);

    bool isPassed = SetExpect(
        previewPlug.SetPreview(
            target,
            overlay,
            reference,
            nullptr,
            &result,
            CropRemovalMode::KeepInside),
        "Box KeepInside preview should be accepted.");

    auto* clippingPlanes = target->GetMapper()->GetClippingPlanes();
    isPassed = SetExpect(
        clippingPlanes && clippingPlanes->GetNumberOfItems() == 6,
        "Box KeepInside preview should install six clipping planes.") && isPassed;

    if (clippingPlanes && clippingPlanes->GetNumberOfItems() == 6) {
        std::array<double, 3> insidePoint = { 0.0, 0.0, 0.0 };
        std::array<double, 3> outsidePoint = { 2.0, 0.0, 0.0 };
        bool isInsideKept = true;
        bool hasOutsideRemoved = false;
        for (int planeIndex = 0; planeIndex < clippingPlanes->GetNumberOfItems(); ++planeIndex) {
            auto* plane = clippingPlanes->GetItem(planeIndex);
            isInsideKept = isInsideKept
                && plane
                && plane->EvaluateFunction(insidePoint.data()) > kPlaneTolerance;
            hasOutsideRemoved = hasOutsideRemoved
                || (plane
                    && plane->EvaluateFunction(outsidePoint.data()) < -kPlaneTolerance);
        }

        isPassed = SetExpect(
            isInsideKept,
            "Box center should stay in the positive half-space of all six planes.") && isPassed;
        isPassed = SetExpect(
            hasOutsideRemoved,
            "A point outside the box should fall into a discarded half-space.") && isPassed;
    }

    previewPlug.ResetPreview(target, overlay);
    isPassed = SetExpect(
        target->GetMapper()->GetNumberOfClippingPlanes() == 0,
        "ResetPreview should remove mesh clipping planes.") && isPassed;
    return isPassed;
}

bool StartPlaneKeepCase()
{
    CropPreviewPlug previewPlug;
    auto target = std::make_shared<CropViewStub>();
    auto reference = std::make_shared<CropViewStub>();
    auto overlay = std::make_shared<CropOverlay>();
    auto result = GetPreviewResult(CropShape::Plane);

    bool isPassed = SetExpect(
        previewPlug.SetPreview(
            target,
            overlay,
            reference,
            nullptr,
            &result,
            CropRemovalMode::KeepInside),
        "Plane KeepInside preview should be accepted.");

    auto* clippingPlanes = target->GetMapper()->GetClippingPlanes();
    isPassed = SetExpect(
        clippingPlanes && clippingPlanes->GetNumberOfItems() == 1,
        "Plane KeepInside preview should install one clipping plane.") && isPassed;
    if (clippingPlanes && clippingPlanes->GetNumberOfItems() == 1) {
        auto* plane = clippingPlanes->GetItem(0);
        std::array<double, 3> positivePoint = { 0.0, 0.0, 1.0 };
        std::array<double, 3> negativePoint = { 0.0, 0.0, -1.0 };
        isPassed = SetExpect(
            plane
                && plane->EvaluateFunction(positivePoint.data()) > kPlaneTolerance
                && plane->EvaluateFunction(negativePoint.data()) < -kPlaneTolerance,
            "Plane KeepInside should retain the normal-positive half-space.") && isPassed;
    }
    return isPassed;
}

bool StartBadShapeCase()
{
    CropPreviewPlug previewPlug;
    auto target = std::make_shared<CropViewStub>();
    auto reference = std::make_shared<CropViewStub>();
    auto overlay = std::make_shared<CropOverlay>();
    auto result = GetPreviewResult(CropShape::Cylinder);

    bool isPassed = SetExpect(
        !previewPlug.SetPreview(
            target,
            overlay,
            reference,
            nullptr,
            &result,
            CropRemovalMode::KeepInside),
        "Unsupported mesh KeepInside geometry should be rejected.");
    isPassed = SetExpect(
        target->GetMapper()->GetNumberOfClippingPlanes() == 0,
        "Rejected mesh geometry should not leave clipping planes behind.") && isPassed;
    return isPassed;
}

    int GetFailCount()
    {
        int failureCount = 0;
        failureCount += StartBoxKeepCase() ? 0 : 1;
        failureCount += StartPlaneKeepCase() ? 0 : 1;
        failureCount += StartBadShapeCase() ? 0 : 1;
        return failureCount;
    }
};

int CropPreviewSuite::GetFailCount() const
{
    return CropPreviewCases().GetFailCount();
}
