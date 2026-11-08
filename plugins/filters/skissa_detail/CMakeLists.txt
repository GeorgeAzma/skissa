set(kritaskissadetail_SOURCES
    SkissaDetail.cpp
    SkissaDetailFilter.cpp
    SkissaDetailWidget.cpp
    SkissaFewer.cpp
    SkissaFewerFilter.cpp
    SkissaLens.cpp
    SkissaLensFilter.cpp
    SkissaNoise.cpp
    SkissaNoiseFilter.cpp
    SkissaPixels.cpp
    SkissaRadial.cpp
    SkissaRadialFilter.cpp
    )
kis_add_library(kritaskissadetail MODULE ${kritaskissadetail_SOURCES})
target_link_libraries(kritaskissadetail kritaui)
install(TARGETS kritaskissadetail DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
