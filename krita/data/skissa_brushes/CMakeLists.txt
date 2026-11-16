# Skissa's brush set, made by packaging/skissa-brushes.py
install(FILES
    Round.kpp Opacity.kpp Flow.kpp Size.kpp Soft.kpp Rough.kpp Blend.kpp Blur.kpp Smear.kpp Fill.kpp Clone.kpp
    Pencil.myb Pencil_prev.png
    DESTINATION ${KDE_INSTALL_DATADIR}/krita/paintoppresets)

# bundles shipped with Skissa that are too large for the repository (Rakurri's brush set), kept beside the build
set(SKISSA_EXTRA_BUNDLES "${CMAKE_SOURCE_DIR}/../krita-deps/bundles" CACHE PATH "Bundles installed with Skissa")
if(EXISTS "${SKISSA_EXTRA_BUNDLES}")
    file(GLOB skissa_extra_bundles "${SKISSA_EXTRA_BUNDLES}/*.bundle")
    install(FILES ${skissa_extra_bundles} DESTINATION ${KDE_INSTALL_DATADIR}/krita/bundles)
endif()
