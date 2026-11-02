set(kritaskissacolor_SOURCES
    SkissaColor.cpp
    SkissaColorFilter.cpp
    SkissaColorWidget.cpp
    )
kis_add_library(kritaskissacolor MODULE ${kritaskissacolor_SOURCES})
target_link_libraries(kritaskissacolor kritaui)
install(TARGETS kritaskissacolor DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
