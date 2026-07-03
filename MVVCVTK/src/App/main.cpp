#define _CRT_SECURE_NO_WARNINGS

#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "Host/VtkAppHostSession.h"

int main()
{
    vtkSMPTools::Initialize();

    VtkAppHostSession session;
    session.Start();

    return 0;
}
