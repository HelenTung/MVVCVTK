#include "CropBridgeTests.h"
#include "Interaction/CropBridge.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <functional>
#include <iostream>

int CropBridgeSuite::GetFailCount() const
{
    int failureCount = 0;
    const auto expect = [&failureCount](bool isExpected, const char* message) {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    };

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(static_cast<float*>(image->GetScalarPointer()), 64, 1.0f);

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
    auto interactor = vtkSmartPointer<vtkRenderWindowInteractor>::New();
    auto camera = vtkSmartPointer<vtkCamera>::New();
    renderer->SetActiveCamera(camera);
    renderWindow->SetOffScreenRendering(1);
    renderWindow->AddRenderer(renderer);
    interactor->SetRenderWindow(renderWindow);

    CropBridge bridge;
    bridge.SetInputImage(image, 1);
    bridge.SetReferenceRenderer(renderer);
    bridge.SetPrimaryInteractor(interactor);
    expect(bridge.SwitchCropBox(), "Bridge should enter box editing.");

    bridge.SetSubmitReloadHandler(
        [camera](vtkSmartPointer<vtkImageData>, std::function<void(bool)>) {
            camera->SetClippingRange(12.0, 24.0);
            return false;
        });
    expect(!bridge.SendSubmit(), "Rejected reload should fail submit.");
    expect(bridge.GetCropActive(), "Rejected reload should preserve editing.");

    bridge.SetSubmitReloadHandler(
        [camera](vtkSmartPointer<vtkImageData>, std::function<void(bool)> complete) {
            camera->SetClippingRange(40.0, 80.0);
            complete(true);
            return true;
        });
    expect(bridge.SendSubmit(), "Accepted reload should accept submit.");
    double restored[2] = { 0.0, 0.0 };
    camera->GetClippingRange(restored);
    expect(restored[0] == 12.0 && restored[1] == 24.0,
        "Successful submit must restore the accepted camera snapshot.");
    expect(!bridge.GetCropActive(), "Successful reload should end the crop session.");
    return failureCount;
}
