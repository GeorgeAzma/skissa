set(kritaskissadetail_SOURCES
    SkissaDetail.cpp
    SkissaDetailFilter.cpp
    SkissaDetailWidget.cpp
    SkissaFewer.cpp
    SkissaFewerFilter.cpp
    SkissaFocus.cpp
    SkissaFocusFilter.cpp
    SkissaHaze.cpp
    SkissaHazeFilter.cpp
    SkissaFollow.cpp
    SkissaInk.cpp
    SkissaInkFilter.cpp
    SkissaLens.cpp
    SkissaLensFilter.cpp
    SkissaLight.cpp
    SkissaLightFilter.cpp
    SkissaNoise.cpp
    SkissaNoiseFilter.cpp
    SkissaPaint.cpp
    SkissaPaintFilter.cpp
    SkissaPixels.cpp
    SkissaRadial.cpp
    SkissaRadialFilter.cpp
    SkissaReplaceFilter.cpp
    SkissaShape.cpp
    SkissaShapeFilter.cpp
    )
kis_add_library(kritaskissadetail MODULE ${kritaskissadetail_SOURCES})
target_link_libraries(kritaskissadetail kritaui)
install(TARGETS kritaskissadetail DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
