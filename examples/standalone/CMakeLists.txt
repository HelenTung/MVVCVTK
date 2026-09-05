add_executable(
    MVVCVTKStandalone
    main.cpp
)
set_target_properties(
    MVVCVTKStandalone
    PROPERTIES
        OUTPUT_NAME MVVCVTK
        PROJECT_LABEL MVVCVTK
        FOLDER "MVVCVTK/Application"
        VS_DEBUGGER_WORKING_DIRECTORY
            "$<TARGET_FILE_DIR:MVVCVTKStandalone>"
)

list(JOIN MVVCVTK_RUNTIME_DIRS ";" _mvvcvtk_runtime_path)
set_property(
    TARGET MVVCVTKStandalone
    PROPERTY VS_DEBUGGER_ENVIRONMENT
        "PATH=${_mvvcvtk_runtime_path};%PATH%"
)
unset(_mvvcvtk_runtime_path)
target_compile_definitions(
    MVVCVTKStandalone
    PRIVATE MVVCVTK_CMAKE_AUTOINIT=1
)
target_link_libraries(
    MVVCVTKStandalone
    PRIVATE
        MVVCVTK::Host
        MVVCVTK::OrthogonalCrop
        MVVCVTK::GapAnalysis
        ${MVVCVTK_VTK_TARGETS}
)
if(MVVCVTK_BUILD_PART_SEGMENTATION)
    target_compile_definitions(
        MVVCVTKStandalone
        PRIVATE MVVCVTK_HAS_PART_SEGMENTATION=1
    )
    target_link_libraries(
        MVVCVTKStandalone
        PRIVATE MVVCVTK::PartSegmentation
    )
endif()
if(MVVCVTK_BUILD_SURFACE_DETERMINATION)
    target_compile_definitions(MVVCVTKStandalone PRIVATE MVVCVTK_HAS_SURFACE_DETERMINATION=1)
    target_link_libraries(MVVCVTKStandalone PRIVATE MVVCVTK::SurfaceDetermination)
endif()
SetTargetDefaults(MVVCVTKStandalone)
vtk_module_autoinit(
    TARGETS MVVCVTKStandalone
    MODULES ${MVVCVTK_VTK_TARGETS}
)
