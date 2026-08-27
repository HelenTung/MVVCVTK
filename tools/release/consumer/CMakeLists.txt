cmake_minimum_required(VERSION 4.2)

project(MVVCVTKSdkConsumer LANGUAGES CXX)

find_package(
    MVVCVTK
    CONFIG REQUIRED
    COMPONENTS
        HostAPI
        FeatureSPI
        Host
        OrthogonalCrop
        GapAnalysis
)

add_executable(
    mvvcvtk_sdk_consumer
    main.cpp
)

target_compile_features(
    mvvcvtk_sdk_consumer
    PRIVATE
        cxx_std_17
)

target_link_libraries(
    mvvcvtk_sdk_consumer
    PRIVATE
        MVVCVTK::Host
        MVVCVTK::OrthogonalCrop
        MVVCVTK::GapAnalysis
)

set(_mvvcvtk_public_headers
    App/AppTypes.h
    App/ViewTypes.h
    App/Services/FeatureViewService.h
    Data/ImageReadTypes.h
    Data/TrustedImageState.h
    Host/CropHostFeature.h
    Host/GapHostFeature.h
    Host/GapHostTypes.h
    Host/HostFeature.h
    Host/Types/HostFeatureViewTypes.h
    Host/Types/HostInputTypes.h
    Host/Types/HostRequest.h
    Host/Types/HostRequestTypes.h
    Host/Types/HostSessionTypes.h
    Host/Types/HostValueTypes.h
    Host/VtkAppHostSession.h
    Interaction/InteractionTypes.h
    OrthogonalCropTypes.h
    Render/Contracts/OverlayService.h
    Render/Contracts/RenderEffect.h
    Render/Contracts/VisualStrategy.h
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/headers")
set(_mvvcvtk_header_sources)
foreach(_mvvcvtk_header IN LISTS _mvvcvtk_public_headers)
    string(MAKE_C_IDENTIFIER "${_mvvcvtk_header}" _mvvcvtk_source_name)
    set(
        _mvvcvtk_source
        "${CMAKE_CURRENT_BINARY_DIR}/headers/${_mvvcvtk_source_name}.cpp"
    )
    file(GENERATE
        OUTPUT "${_mvvcvtk_source}"
        CONTENT "#include <${_mvvcvtk_header}>\n"
    )
    list(APPEND _mvvcvtk_header_sources "${_mvvcvtk_source}")
endforeach()
add_library(
    mvvcvtk_installed_headers
    OBJECT
    ${_mvvcvtk_header_sources}
)
target_link_libraries(
    mvvcvtk_installed_headers
    PRIVATE
        MVVCVTK::HostAPI
        MVVCVTK::FeatureSPI
)
unset(_mvvcvtk_header)
unset(_mvvcvtk_header_sources)
unset(_mvvcvtk_public_headers)
unset(_mvvcvtk_source)
unset(_mvvcvtk_source_name)
